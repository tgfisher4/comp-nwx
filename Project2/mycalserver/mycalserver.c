/*
Group: Graham Fisher, Tommy Gallagher, Jason Brown
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdbool.h>
#include <pthread.h>
#include <libgen.h>
#include <sys/stat.h>
#include <math.h>

#include <jansson.h> // the JSON parsing library we chose to use: https://jansson.readthedocs.io/en/latest | https://github.com/akheron/jansson

#define BACKLOG 10   // how many pending connections queue will hold
#define CONFIG_PATH ".mycal"
#define DATA_PATH "data/all_data.json"

int MONTH_TO_N_DAYS[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; // ignores leap years

json_t *global_database; // global data shared among threads
pthread_rwlock_t global_database_lock = PTHREAD_RWLOCK_INITIALIZER; // rd/wr lock associated with global_database

struct client_thread_args {
    int sockfd;
};

char *get_port_from_cfg(char *cfg_pathname);
int mkpath(char *dir_path, mode_t mode);
json_t *load_data(char *data_path);
void save_data(char *data_path, json_t *database, pthread_rwlock_t lock, int *exit_status);
void *run_client_thread(void *void_args);
void handle_client(int sockfd, json_t *database, pthread_rwlock_t lock);
json_t *get_next_request(int sockfd);
json_t *dispatch(json_t *request, json_t *database, pthread_rwlock_t lock);
char *add_event(char **event_id_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *date, const char *time, const char *duration_min, const char *name, const char *description, const char *location);
char *remove_event(json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *event_id);
char *update_event(json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *event_id, const char* field, const char *value);
char *get_events_on_date(json_t **results_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *date);
char *get_events_in_range(json_t **results_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *start_date, const char *end_date);


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

void sigint_handler(int s){
    save_data(DATA_PATH, global_database, global_database_lock, &s);
}

int main(int argc, char* argv[]){
    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    struct sigaction sa;
    int yes=1;
    char s[INET6_ADDRSTRLEN];
    int rv;
    bool multithread = false;

    if( argc == 2 && !strcmp(argv[1], "-mt") ){
        multithread = true;
    }
    else if( argc != 1 ){
        fprintf(stderr, "[Error] Usage: %s [-mt]\n", argv[0]);
    }

    char *port = get_port_from_cfg(CONFIG_PATH);
    if( !port ){
        fprintf(stderr, "[Error] Could not read port from config file (file couldn't be opened or 'port' field was not found)\n");
        return 1;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "[Error] Failed to getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("[Error] Failed to create socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                sizeof(int)) == -1) {
            perror("[Error] Failed to setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("[Error] Failed to bind a socket");
            continue;
        }

        break;
    }

    freeaddrinfo(servinfo); // all done with this structure
    free(port); // all done with this string repr of port

    if (p == NULL)  {
        fprintf(stderr, "[Error] Failed to bind any socket");
        exit(1);
    }

    if (listen(sockfd, BACKLOG) == -1) {
        perror("[Error] Failed to listen");
        exit(1);
    }

    // Set up SIGINT handler
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask); // the triggering signal is blocked by default
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("[Error] Failed to set sigaction");
        exit(1);
    }

    global_database = load_data(DATA_PATH);

    printf("[INFO] Waiting for connections...\n");

    while(1) {  // main accept() loop
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            perror("[Error] Failed to accept connection");
            continue;
        }

        inet_ntop(their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),
            s, sizeof s);
        printf("[INFO] Accepted connection from %s\n", s);

        if( multithread ){
            // create run_client_thread_args object, fill with args
            pthread_t new_thread;
            struct client_thread_args args;
            args.sockfd = new_fd;
            pthread_create(&new_thread, NULL, run_client_thread, &args); // do anything with return value?
        }
        else{
            handle_client(new_fd, global_database, global_database_lock);
        }
    }
    return 0;
}

char *get_port_from_cfg(char *cfg_pathname){
    /*** Read port from cfg file ***/
    
    // parse json cfg file
    FILE *cfg_file = fopen(cfg_pathname, "r");
    json_t *cfg_contents = json_loadf(cfg_file, 0, NULL); 
    if( !cfg_contents ){
        return NULL;
    }
    // access correct field of the struct
    json_t *port_json = json_object_get(cfg_contents, "port");
    if( !port_json ){
        return NULL;
    }

    const char *port = json_string_value(port_json); // need to be const char *?
    // copy over port so it is not lost when cfg_contents destroyed
    char *port_to_return = malloc(strlen(port)+1); // caller needs to free returned string
    sprintf(port_to_return, "%s", port); // sprintf adds a nul terminator

    // cleanup
    json_decref(cfg_contents); // frees port_json, too (borrowed reference)
    fclose(cfg_file);

    return port_to_return;
}

int mkpath(char *dir_path, mode_t mode){
    if( !dir_path ){
        errno = EINVAL;
        return -1;
    }
    // base case: current directory or root
    if( !strcmp(dir_path, ".") || !strcmp(dir_path, "/") ){
        return 0;
    }

    char *dir_path_copy = strdup(dir_path);
    if( !dir_path_copy ){
        return -1;
    }

    int rc;
    rc = mkpath(dirname(dir_path_copy), mode);
    free(dir_path_copy);
    if( rc < 0 ){
        return -1;
    }
    rc = mkdir(dir_path, mode);
    if( rc < 0 && errno != EEXIST ){
        return -1;
    }
    return 0;
}

json_t *load_data(char *data_path){
    /*** Load data from json file ***/

    // Try to load from data_path
    FILE *data_file = fopen(data_path, "r");
    // If data loading fails, try to create directories along the way
    // I think it would make more sense to be lazier and create dir just before saving but instructions suggest this approach
    if( !data_file && errno == ENOENT ){ 
        // Parse until slash
        // abc/def
        char *last_slash = strrchr(data_path, '/');
        char* dir_path = malloc(last_slash - data_path + 1);
        snprintf(dir_path, last_slash - data_path + 1, "%s", data_path); // writes a nul terminator
        if( mkpath(dir_path, S_IRWXU | S_IRWXO) < 0 ){
            perror("[Error] Failed to make path to data save location");
            exit(1);
        }
        free(dir_path);
        return json_object(); // no data to load
    }
    json_t *data = json_loadf(data_file, 0, NULL);
    fclose(data_file);
    return data;

    /*** Internal data storage format ***/
    // note: jansson uses a hash table underneath so we can use its JSON objects and have it handle hashing, plus be ready to encode when time comes
    /*
        {
            <calendar name>: {
                <date>: [
                    {
                        time: <HHMM>,
                        duration: <num min>,
                        name: <string>,
                        description: <string>,
                        location: <string>,
                        removed: <bool>
                    }
                ]
            }
        }
    */
    // Note that we use the date as a hash key so that we can index events quickly and easily based on date to serve get and getrange requests
    // Note that we will use the following scheme for eventids: "<date><idx>" where idx is the 0-based index of the event within the date (since, according to assignment instructions, we don't have to worry about the server needing to work on the same calendar from multiple threads, we can treat the server as a serialization points, i.e., assume that events are put into the data structure in the same order)
}

void save_data(char *data_path, json_t *database, pthread_rwlock_t lock, int *exit_status){
    // Potential problems with multithreading:
    //  - two threads have failures, so each tries to save and exit
    //  - while the first writes, the second cannot since file has lock
    //  - however, once first releases lock, what if second runs, erases file, then first runs and exits
    //  - so, if exiting, we do it before closing the file so that someone else can't modify the file before we erase it
    FILE *data_file = fopen(data_path, "w");
    if( !data_file ){
        fprintf(stderr, "[Error] Unable to save database: could not open save file\n");
    }
    pthread_rwlock_rdlock(&lock);
    json_dumpf(database, data_file, JSON_COMPACT);
    pthread_rwlock_unlock(&lock);
    if( exit_status ) exit(*exit_status);
    fclose(data_file);
}

void save_and_exit(int exit_status){ save_data(DATA_PATH, global_database, global_database_lock, &exit_status); }

void *run_client_thread(void *void_args){
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL); // only the main thread should receive SIGINT (and save data in response)
    struct client_thread_args *args = (struct client_thread_args *)void_args;
    handle_client(args->sockfd, global_database, global_database_lock);
    return NULL;
}

void handle_client(int sockfd, json_t *database, pthread_rwlock_t lock){
    while(true){
        json_t *next_request = get_next_request(sockfd);
        if( !next_request ){
            printf("[Info] Connection with client broken.\n");
            break;
        }
        json_t *result;
        if( json_is_string(next_request) ){ //  if it's a string, its an error message
            result = json_object();
            json_object_set_new(result, "success", json_false());
            json_object_set_new(result, "error", json_copy(next_request));
        }
        else{
            result = dispatch(next_request, database, lock);
        }
        int rc = json_dumpfd(result, sockfd, JSON_COMPACT); // takes care of sending over TCP (streaming) socket
        if( rc < 0 ){
            fprintf(stderr, "[Error] Problem sending result back to client. Aborting connection...\n");
            break;
        }
        if( send(sockfd, "\n", 1, 0) != 1 ){
            fprintf(stderr, "[Error] Problem sending result back to client. Aborting connection...\n");
            break;
        }

        json_decref(next_request);
        json_decref(result);
    }
    close(sockfd);
}

json_t *get_next_request(int sockfd){
    static char *buffer = NULL;
    if( !buffer ){
        buffer = malloc(BUFSIZ);
    }
    if( !buffer ){
        fprintf(stderr, "[Error] Memory allocation failed: %s. Aborting...\n", strerror(errno));
        save_and_exit(1);
    }
    static int space_remaining = BUFSIZ; // essentially tracks end of meaningful data in buffer
    // TODO: convert between host and network byte order?
    int current_bufsiz_multiple = 1; // resets to 1 on every invocation
    do {
        int space_used = current_bufsiz_multiple * BUFSIZ - space_remaining;
        char *unused_buffer_start = buffer + space_used;
        /*
        printf("[Debug] %d of %d bytes remaining in buffer\n", space_remaining, current_bufsiz_multiple * BUFSIZ);
        char *print_buffer = malloc(space_used + 1);
        snprintf(print_buffer, space_used + 1, "%s", buffer);
        printf("[Debug] Start of buffer|%s|End of buffer\n", print_buffer);
        free(print_buffer);
        */

        // If the valid portion of the buffer contains a newline, the next request fits in the current buffer.
        // Parse request and save reference in buffer so we can pick up from here next time (in case we received 2 full requests at once)
        char *newline = memchr(buffer, '\n', unused_buffer_start - buffer);
        if( newline ){
            // Parse JSON up to newline
            // Parse now so we can throw out the string we parsed from: returning a string might be more general, but would require allocating space for the copied request so we can free the buffer (certainly doable but adds complexity which is unneeded here).
            json_t *to_return = json_loadb(buffer, newline - buffer, 0, NULL);
            if( !to_return ){
                to_return = json_string("Invalid request: could not parse as JSON");
            }

            // Bookkeep for next time
            // Because we check after every BUFSIZ-sized chunk, we can be sure that any valid data beyond the newline will fit into a BUFSIZ-size buffer
            int n_bytes_to_preserve = space_used - (newline - buffer) - 1; // valid data left in buffer beyond newline: valid - parsed - 1 (newline): < BUFSIZ
            char *small_buffer = malloc(BUFSIZ);
            if( !small_buffer ){
                fprintf(stderr, "[Error] Memory allocation failed: %s. Aborting...\n", strerror(errno));
                save_and_exit(1);
            }
            strncpy(small_buffer, newline + 1, n_bytes_to_preserve);
            free(buffer); // release space after each request: if we keep adding onto same buffer we will consume lots of memory we aren't using
            buffer = small_buffer;
            space_remaining = BUFSIZ - n_bytes_to_preserve; // everything beyond what we will preserve is free buffer space
            /*
            printf("[Debug] Preserving %d characters, so that there is %d space remaining in the buffer (we expect %d = %d + %d)\n", n_bytes_to_preserve, space_remaining, BUFSIZ, n_bytes_to_preserve, space_remaining);
            char *preserved_contents = malloc(n_bytes_to_preserve + 1);
            snprintf(preserved_contents, n_bytes_to_preserve+1, "%s", buffer);
            printf("[Debug] Preserved buffer contents: START|%s|END\n", preserved_contents);
            free(preserved_contents);
            */
            return to_return;
        }

        // If buffer full, allocate more space: keep it contiguous so we can JSON parse it all together
        if( space_remaining == 0 ){
            current_bufsiz_multiple += 1;
            buffer = realloc(buffer, current_bufsiz_multiple * BUFSIZ);
            if( !buffer ){
                fprintf(stderr, "[Error] Memory allocation failed: %s. Aborting...\n", strerror(errno));
                save_and_exit(1);
            }
            space_remaining = BUFSIZ;
        }

        // Receive more data to try to find end of request
        int got = recv(sockfd, unused_buffer_start, space_remaining, 0);
        if( got < 0 ){
            perror("[Error] Could not receive from client");
            return NULL;
        }
        if( got == 0 ){
            return NULL;
        }
        space_remaining -= got;
    } while(true);
}

json_t *dispatch(json_t *request, json_t *database, pthread_rwlock_t lock){
    json_t *result = json_object();
    const char *command = json_string_value(json_object_get(request, "command"));
    if( !(command) ){
        // BAD REQUEST: NO COMMAND
        json_object_set_new(result, "success", json_false());
        json_object_set_new(result, "error", json_string("Request payload is missing 'command' field"));
        json_object_set_new(result, "identifier", json_string("XXXX"));
        return result;
    }
    json_object_set_new(result, "command", json_copy(json_object_get(request, "command"))); // creates a new reference so we don't have to spend 2 extra lines first fetching and then increffing request.command

    const char *calendar_name;
    if( !(calendar_name = json_string_value(json_object_get(request, "calendar"))) ){
        // BAD REQUEST: NO CALENDAR
        json_object_set_new(result, "success", json_false());
        json_object_set_new(result, "error", json_string("Request payload is missing 'calendar' field"));
        json_object_set_new(result, "identifier", json_string("XXXX"));
        return result;
    }
    json_object_set_new(result, "calendar", json_copy(json_object_get(request, "calendar"))); // creates a new reference so we don't have to spend 2 extra lines first fetching and then increffing request.calendar

    if( !strcmp(command, "add") ){
        const char *calendar_name, *date, *time, *duration_min, *name, *description = NULL, *location = NULL;
        int rc = json_unpack(request,
            "{ s:s, s:s, s:s, s:s, s:s, s:s,"
            "  s?:s, s?:s }",
            "command", &command,
            "calendar", &calendar_name,
            "date", &date,
            "time", &time,
            "duration", &duration_min,
            "name", &name,
            "description", &description,
            "location", &location
        );
        if( rc < 0 ){
            // BAD REQUEST: WRONG FORMAT
            json_object_set_new(result, "success", json_false());
            json_object_set_new(result, "error", json_string("Invalid request format: expected keys of calendar, date, time, duration, name, and, optionally, description and location, each with a string value."));
            json_object_set_new(result, "identifier", json_string("XXXX"));
            return result;
        }
        char *added_event_id;
        char *error = add_event(
            &added_event_id,
            database, lock,
            calendar_name,
            date, time, duration_min,
            name, description, location
        );
        // Finish constructing result
        json_object_set_new(result, "success", json_boolean(!error));
        if( error ){
            json_object_set_new(result, "error", json_string(error));
            json_object_set_new(result, "identifier", json_string("XXXX"));
        }
        else {
            json_object_set_new(result, "identifier", json_string(added_event_id));
            // Inspecting the jansson source, json_string makes a copy of the string input, so it is safe to free this here
            free(added_event_id);
        }
        return result;
    }
    if( !strcmp(command, "remove") ){
        char *calendar_name, *event_id;
        int rc = json_unpack(request,
            "{ s:s, s:s, s:s }",
            "command", &command,
            "calendar", &calendar_name,
            "event_id", &event_id
        );
        if( rc < 0 ){
            // BAD REQUEST: WRONG FORMAT
            json_object_set_new(result, "success", json_false());
            json_object_set_new(result, "error", json_string("Invalid request format: expected keys of calendar and event_id, each with a string value."));
            json_object_set_new(result, "identifier", json_string("XXXX"));
            return result;
        }

        char *error = remove_event(
            database, lock,
            calendar_name,
            event_id
        );
        // Finish constructing result
        json_object_set_new(result, "success", json_boolean(!error));
        if( error ){
            json_object_set_new(result, "error", json_string(error));
            json_object_set_new(result, "identifier", json_string("XXXX"));
        }
        else {
            json_object_set_new(result, "identifier", json_string(event_id));
        }
        return result; 
    }
    if( !strcmp(command, "update") ){
        char *calendar_name, *event_id, *field, *value;
        int rc = json_unpack(request,
            "{ s:s, s:s, s:s, s:s, s:s }",
            "command", &command,
            "calendar", &calendar_name,
            "event_id", &event_id,
            "field", &field,
            "value", &value
        );
        if( rc < 0 ){
            // BAD REQUEST: WRONG FORMAT
            json_object_set_new(result, "success", json_false());
            json_object_set_new(result, "error", json_string("Invalid request format: expected keys of calendar, event_id, field, and value, each with a string value."));
            json_object_set_new(result, "identifier", json_string("XXXX"));
            return result;
        }
        char *error = update_event(
            database, lock,
            calendar_name,
            event_id,
            field,
            value
        );
        // Finish constructing result
        json_object_set_new(result, "success", json_boolean(!error));
        if( error ){
            json_object_set_new(result, "error", json_string(error));
            json_object_set_new(result, "identifier", json_string("XXXX"));
        }
        else {
            json_object_set_new(result, "identifier", json_string(event_id));
        }
        return result; 
    }
    if( !strcmp(command, "get") ){
        char *calendar_name, *date;
        int rc = json_unpack(request,
            "{ s:s, s:s, s:s }",
            "command", &command,
            "calendar", &calendar_name,
            "date", &date
        );
        if( rc < 0 ){
            // BAD REQUEST: WRONG FORMAT
            json_object_set_new(result, "success", json_false());
            json_object_set_new(result, "error", json_string("Invalid request format: expected keys of calendar and date, each with a string value."));
            //json_object_set_new(result, "identifier", json_string("XXXX"));
            return result;
        }
        json_t *events_on_date;
        char *error = get_events_on_date(
            &events_on_date,
            database, lock,
            calendar_name,
            date
        );
        // Finish constructing result
        json_object_set_new(result, "success", json_boolean(!error));
        if( error ){
            json_object_set_new(result, "error", json_string(error));
            //json_object_set_new(result, "identifier", json_string("XXXX"));
        }
        else {
            json_object_set_new(result, "data", events_on_date); // give ownership of queried data events_on_date to result
        }
        return result; 
    }
    if( !strcmp(command, "getrange") ){
        char *calendar_name, *start_date, *end_date;
        int rc = json_unpack(request,
            "{ s:s, s:s, s:s, s:s }",
            "command", &command,
            "calendar", &calendar_name,
            "start_date", &start_date,
            "end_date", &end_date
        );
        if( rc < 0 ){
            // BAD REQUEST: WRONG FORMAT
            json_object_set_new(result, "success", json_false());
            json_object_set_new(result, "error", json_string("Invalid request format: expected keys of calendar, start_date, and end_date, each with a string value."));
            //json_object_set_new(result, "identifier", json_string("XXXX"));
            return result;
        }
        json_t *events_on_dates;
        char *error = get_events_in_range(
            &events_on_dates,
            database, lock,
            calendar_name,
            start_date,
            end_date
        );
        // Finish constructing result
        json_object_set_new(result, "success", json_boolean(!error));
        if( error ){
            json_object_set_new(result, "error", json_string(error));
            //json_object_set_new(result, "identifier", json_string("XXXX"));
        }
        else {
            json_object_set_new(result, "data", events_on_dates); // give ownership of queried data events_on_dates to result
        }
        return result; 
    }

    // Else, command invalid
    char *error_msg = "Invalid command (expected one of: add, update, remove, get, getrange)";
    json_object_set_new(result, "error", json_string(error_msg));
    json_object_set_new(result, "success", json_false());
    json_object_set_new(result, "identifier", json_string("XXXX"));
    return result;
}

/*** Data utilities ***/
bool validate_date(const char* date, int *month_out, int *day_out, int *year_out){
    if( strlen(date) != 6 ){
        return false;
    }
    char month_str[3], day_str[3], year_str[3]; 
    sscanf(date, "%2s%2s%2s", month_str, day_str, year_str); // sscanf adds NULs to parsed strings
    int month = atoi(month_str);
    int day = atoi(day_str);
    int year = atoi(year_str);

    if( month <= 0 || month > 12 ) return false;
    if( day <= 0 || day > MONTH_TO_N_DAYS[month] ) return false;
    // Any year is valid

    if( month_out ) *month_out = month;
    if( day_out ) *day_out = day;
    if( year_out ) *year_out = year;
    return true;
}

bool validate_time(const char *time, int *hour_out, int *minute_out){
    if( strlen(time) != 4 ){
        return false;
    }
    char hour_str[3], minute_str[3]; 
    sscanf(time, "%2s%2s", hour_str, minute_str); // sscanf adds NULs to parsed strings
    int hour = atoi(hour_str);
    int minute = atoi(minute_str);

    if( hour < 0 || hour > 23 ) return false;
    if( minute < 0 || minute > 59 ) return false;

    if( hour_out ) *hour_out = hour;
    if( minute_out ) *minute_out = minute;
    return true;
}

bool str_to_int(const char *str, int *int_out){
    char *end_of_int;
    int res = strtol(str, &end_of_int, 10);
    if( end_of_int != str + strlen(str) ){
        return false;
    }
    if( int_out ) *int_out = res;
    return true;
}

int parse_event_id(const char *event_id, char *date, int *id_on_date){
    // date and id_on_date MUST be preallocated with spaces of 7 chars for date and 1 int for id_on_date
    strncpy(date, event_id, 6);
    date[6] = '\0';
    *id_on_date = atoi(event_id + 7);
    return 0;
}

/*** Action function return schemes ***/
// Things we need to communicate back to caller
//  - success?
//  - err msg (string)
//  - result (type varies)
// However, success can be inferred from the absence of an error.
// Therefore, to facilitate easy error-checking, I suggest that every function return their error message (or NULL on success).
// This leaves the result to be passed back via a parameter.
char *add_event(char **event_id_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *date, const char *time, const char *duration_min, const char *name, const char *description, const char *location){
    // Use strings for internal storage of ALL fields for simplicity

    /*** Validation ***/
    if( !validate_date(date, NULL, NULL, NULL) ){
        return "Invalid date (expected 'MMDDYY')";
    }
    if( !validate_time(time, NULL, NULL) ){
        return "Invalid time (expected 'HHMM')";
    }
    if( !str_to_int(duration_min, NULL) ){
        return "Invalid duration (expected integer-convertible string)";
    }

    pthread_rwlock_wrlock(&lock);
    // If no event has been added to calendar yet, create a calendar object
    json_t *calendar = json_object_get(database, calendar_name);
    if( !calendar ){
        calendar = json_object();
        json_object_set_new(database, calendar_name, calendar); // ref stolen by "new": no need to decref later
    }
    // If not event has been added to this date in the calendar yet, create a date array
    json_t *cal_date = json_object_get(calendar, date);
    if( !cal_date ){
        cal_date = json_array();
        json_object_set_new(calendar, date, cal_date); // ref stolen by "new": no need to decref later
    }

    /*** Now that we have a cal_date array, and append the new event ***/
    int id_on_date = json_array_size(cal_date); // save for later

    // Create a new event and add the expected fields
    json_t *new_event = json_object();
    json_object_set_new(new_event, "time", json_string(time));
    json_object_set_new(new_event, "duration", json_string(duration_min));
    json_object_set_new(new_event, "name", json_string(name));
    json_object_set_new(new_event, "description", description ? json_string(description): json_null());
    json_object_set_new(new_event, "location", location ? json_string(location): json_null());
    json_object_set_new(new_event, "removed", json_false());
    
    json_array_append_new(cal_date, new_event); // hands off reference to calendar
    pthread_rwlock_unlock(&lock);

    if( event_id_out ){
        // Construct event id from date and id_on_date
        // TODO: this event_id will be embedded in a json_t which will later be decref'd: does that free this string? If not, does it copy so that we can free this after returning?
        int id_len = id_on_date > 0 ? ceil(log10(id_on_date)) : 1;
        char *event_id = malloc(6 + 1 + id_len + 1); // 6 = len(MMDDYY), 1 = len(":"), 1 = len("\0")
        sprintf(event_id, "%s:%d", date, id_on_date);

        *event_id_out = event_id;
    }

    return NULL;
}

char *remove_event(json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *event_id){
    // TODO: do I need to get access to old value of "removed" and decref?
    // I wouldn't think so: I'd think the object would decref the old value for me since it owns it
    char date[7]; int id_on_date;
    parse_event_id(event_id, date, &id_on_date);

    pthread_rwlock_wrlock(&lock);
    json_t *event = json_array_get(
        json_object_get(json_object_get(database, calendar_name), date),
        id_on_date
    );
    json_object_del(event, "removed"); // decref's the bool ref to manage memory
    int rc = json_object_set_new(event, "removed", json_true());
    pthread_rwlock_unlock(&lock);

    if( rc < 0 ) return "No such event exists on this calendar";
    return NULL;
}

char *update_event(json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *event_id, const char *field, const char *value){
    char date[7]; int id_on_date;
    parse_event_id(event_id, date, &id_on_date);

    /*** Validate field and value ***/
    if( !strcmp(field, "date") ){
        if( !validate_date(value, NULL, NULL, NULL) ){
            return "Invalid date (expected 'MMDDYY')";
        }
    }
    else if( !strcmp(field, "time") ){
        if( !validate_time(value, NULL, NULL) ){
            return "Invalid time (expected 'HHMM')";
        }
    }
    else if( !strcmp(field, "duration") ){
        if( !str_to_int(value, NULL) ){
            return "Invalid duration (expected integer-convertible string)";
        }
    }
    else if( !strcmp(field, "name") || !strcmp(field, "description") || !strcmp(field, "location") ){
        // Anything is valid
    }
    else
        return "Field is invalid (must be one of: date, time, duration, name, description, location)";

    /*** Perform update ***/
    pthread_rwlock_wrlock(&lock);
    json_t *event = json_array_get(
        json_object_get(json_object_get(database, calendar_name), date),
        id_on_date
    );
    json_object_del(event, field); // decref count of the previous value. May fail if field is optional and was not set previously
    int rc = json_object_set_new(event, field, json_string(value));
    pthread_rwlock_unlock(&lock);
    if( rc < 0 ) return "No such event exists on this calendar";
    return NULL;
}

char *get_events_on_date(json_t **results_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *date){
    if( !validate_date(date, NULL, NULL, NULL) ){
        return "Date invalid (expected MMDDYY)";
    }

    // No need to fetch results if user doesn't want them
    if( !results_out ) return NULL;

    pthread_rwlock_rdlock(&lock);

    // Getting all events on a date is very easy thanks to our data structure
    json_t *events_on_date = json_object_get(json_object_get(database, calendar_name), date);
    // If calendar does not exist (i.e., has no events) or this date does not exist in the calendar (i.e., has no events), return an empty array (signifies no events)
    if( !events_on_date ) events_on_date = json_array();
    // However, to account for our 'removed' field, we must filter out removed events before returning the results
    json_t *results;
    if( json_array_size(events_on_date) ){
        results = json_array();
        json_t *true_obj = json_true();
        int index; json_t *value;
        json_array_foreach(events_on_date, index, value){
            if( !json_equal(json_object_get(value, "removed"), true_obj) ){
                json_t *event_to_append = json_deep_copy(value);
                json_object_del(event_to_append, "removed");
                json_array_append_new(results, event_to_append);
            }
        }
        json_decref(true_obj);
    }
    else{
        // in the case that the event list is empty, we created a new array which we want to embed in the result so that it is decref'd and freed later.
        results = events_on_date;
    }
    pthread_rwlock_unlock(&lock);

    // Wrap in an object to tack on numevents field
    json_t *to_return = json_object();
    json_object_set_new(to_return, "numevents", json_integer(json_array_size(results))); // hand over reference
    json_object_set_new(to_return, "events", results); // hand over reference

    *results_out = to_return; // caller will need to decref to free everything

    return NULL;
}


char *get_events_in_range(json_t **results_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *start_date, const char *end_date){
    // Qs:
    //  - start/stop inclusive or exclusive?
    //  - what is output format (are all events contained in one object? are events nested into objects depending on the day?)? it includes a numdays field? why? Should it be a map of date to a list of events? Or just an array of arrays of events, where the caller can determine which fall on which day based on order and start date
    int month, day, year;
    if( !validate_date(start_date, &month, &day, &year) ){
        return "Start date invalid (expected MMDDYY)";
    }
    int end_month, end_day, end_year;
    if( !validate_date(end_date, &end_month, &end_day, &end_year) ){
        return "End date invalid (expected MMDDYY)";
    }
    bool start_after_end = (end_year < year) || (end_year == year && end_month < month) || (end_year == year && end_month == month && end_day < day);
    if( start_after_end ){
        return "End date precedes start date (expected end date to follow start date)";
    }

    // No need to fetch results if user doesn't want them
    if( !results_out ) return NULL;

    // No locking because we just call get_events_on_date which does it for us
    json_t *date_to_events = json_object();
    int num_days = 0;
    // TODO: this setup currently uses an inclusive start and exclsuive stop (as is a common paradigm) - is this ok?
    for(; !(day == end_day && month == end_month && year == end_year); num_days++ ){
        char date[7];
        sprintf(date, "%02d%02d%02d", month, day, year);
        json_t *events_today;
        get_events_on_date(&events_today, database, lock, calendar_name, date);
        json_object_set_new(date_to_events, date, events_today); // hand reference to to_return
        /*
        char *events_today_json = json_dumps(events_today, 0);
        if( !events_today_json ) printf("there was an error printing events_today...\n");
        printf("setting key %s to %s\n", date, events_today_json);
        free(events_today_json);
        */

        // Advance to next day, ignoring leap years
        day = (day % MONTH_TO_N_DAYS[month]) + 1;
        bool is_new_month = day == 1;
        if( is_new_month ) month = (month % 12) + 1;
        bool is_new_year = is_new_month && month == 1;
        if( is_new_year ) year = (year % 99) + 1; 
    }

    json_t *to_return = json_object();
    json_object_set_new(to_return, "numdays", json_integer(num_days)); // hand reference over
    json_object_set_new(to_return, "days", date_to_events); // hand reference over
    *results_out = to_return; // caller will need to decref to free everything

    return NULL;
}
