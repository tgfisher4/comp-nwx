/*
** server.c -- a stream socket server demo
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

#include <jansson.h> // the JSON parsing library we chose to use: https://jansson.readthedocs.io/en/latest | https://github.com/akheron/jansson

#define BACKLOG 10   // how many pending connections queue will hold
#define filename_len_len sizeof(uint16_t)
#define file_len_len sizeof(uint32_t)
#define CONFIG_PATH .mycal

int MONTH_TO_N_DAYS = [0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]; // ignores leap years


void sigchld_handler(int s)
{
    (void)s; // quiet unused variable warning

    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int client_handler(int clientsockfd);

char *get_port_from_cfg(char *cfg_pathname){
    /*** Read port from cfg file ***/

    // get cfg file size
    FILE *cfg_file = fopen(filename, "r");
    json_t *cfg_contents = json_loadf(cfg_file, 0, NULL); 
    if( !cfg_contents ){
        perror("[Error] Could not open config file"); // TODO: prolly don't throw errors from helper fxn, just return -1 and let fxn above check errno
        return -1;
    }
    // access correct field of the struct
    json_t *port_json = json_object_get(cfg_contents, "port");
    if( !port_json ){
        // error: field not found
    }
    if( !json_is_integer(port_json) ){
        // error: port should have int type
    }
    int port = json_integer_value(port_json);
    // cleanup
    json_decref(cfg_contents);
    json_decref(port_json);
    fclose(cfg_file);

    // convert int to string
    char *port_string = malloc(ceil(log10(port)) + 1);
    sprintf(port_string, "%i", port); // sprintf adds a nul terminator
    return port_string;
}

#define DATA_LOCATION data/all_data.json

json_t *load_data(char *data_path){
    // try to load data
    FILE *data_file = fopen(data_path);
    if( !data_file && errno == ENOENT ){
        // create directory if doesn't exist: I think it would make more sense to be lazier and create dir just before saving but instructions suggest this approach
        // parse until slash
        char *last_slash = strrchr(data_path, '/');
        char* dir_path = malloc(last_slash - data_path + 1);
        strncpy(dir_path, data_path, last_slash - data_path);
        dir_path[last_slash - data_path] = '\0';
        mkdir(data_path, S_IRWXU);
        // check failure?
    }
    json_t *data = json_loadf(data_file);
    // TODO: integrity check on data after loading?
    return data;

    // IDEA for data formatting :
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

void save_data(char *data_path, json_t *data){
    // TODO: integrity check on data before saving?

    FILE *data_file = fopen(data_path, 'w');
    json_dumpf(data, data_file, JSON_COMPACT);
}

bool str_to_int(char *str, int *int_out){
    char *end_of_int;
    int res = strtol(str, &end_of_int, 0);
    if( end_of_int != duration_min + strlen(duration_min) ){
        return false;
    }
    if( int_out ) *int_out = res;
    return true;
}

/*** Action function return schemes ***/
// Things we need to communicate back to caller
//  - success?
//  - err msg (string)
//  - result (any type)
// However, success can be inferred from the absence of an error.
// Therefore, to facilitate easy error-checking, I suggest that every function return their error message (or NULL on success).
// This leaves the result to be passed back via a parameter.
char *add_event(char **event_id_out, json_t *data, char *calendar_name, char *date, char *time, char *duration_min, char *name, char *description, char *location){
    // Use strings for internal storage of ALL fields for simplicity

    /*** Validation ***/
    // TODO: how to actually return these
    if( !validate_date(date, NULL, NULL, NULL) ){
        return "invalid date (expected MMDDYY)";
    }
    if( !validate_time(time, NULL, NULL) ){
        return "invalid time (expected HHMM)";
    }
    if( !str_to_int(duration_min, NULL) ){
        return "invalid duration (expected integer)";
    }
    
    /*** If no event has been added to date yet, create a date array ***/
    json_t *calendar = json_object_get(data, calendar_name); // returns a "borrowed" reference: no need to worry about manual ref counting
    // Initialize new array for <date>
    json_t *new_date = json_object();
    json_object_set_new(new_date, date, json_array());
    // Attempt to insert new <date> array: calendar will only be updated (i.e., <date> set to an empty array) if there is currently no field called <date>
    json_object_update_missing_new(calendar, new_date); // new_date is stolen by "new": no need to decref

    /*** Now that date array definitely exists, grab it and append the new event ***/
    json_t *cal_date = json_object_get(calendar, date);
    int id_on_date = json_array_size(cal_date); // save for later

    // Create a new event and add the expected fields
    json_t *new_event = json_object();
    json_object_set_new(new_event, "time", json_string(time));
    json_object_set_new(new_event, "duration_min", json_string(duration_min));
    json_object_set_new(new_event, "name", json_string(name));
    json_object_set_new(new_event, "description", description ? json_string(description): json_null());
    json_object_set_new(new_event, "location", location ? json_string(location): json_null());
    json_object_set_new(new_event, "removed", json_false());
    
    json_array_append_new(cal_date, new_event); // hands off reference to calendar

    if( event_id_out ){
        // Construct event id from date and id_on_date
        char *event_id = malloc(6 + ceil(log10(id_on_date)) + 1); // 6 = len(MMDDYY), 1 = len("\0")
        sprintf(event_id, "%s:%d", date, id_on_date);

        *event_id_out = event_id;
    }

    return NULL;
}

int parse_event_id(char *event_id, char *date, int *id_on_date){
    // date and id_on_date MUST be preallocated with spaces of 7 chars for date and 1 int for id_on_date
    // TODO: validate event_id and return error if invalid?
    strncpy(date, event_id, 6);
    date[6] = '\0';
    *id_on_date = atoi(event_id + 7);
    return 0;
}

char *remove_event(json_t *data, char *calendar_name, char *event_id){
    // TODO: do I need to get access to old value of "removed" and decref?
    char date[7], int id_on_date;
    parse_event_id(event_id, date, &id_on_date);

    int rc = json_object_set_new(
        json_array_get(
            json_object_get(json_object_get(data, calendar_name), date),
            id_on_date
        ),
        "removed",
        json_true()
    );
    if( rc < 0 ) return "no such event exists on this calendar";
    return NULL;
}

char *update_event(json_t *data, char *calendar_name, int event_id, char* field, char *value){
    char date[7], int id_on_date;
    parse_event_id(event_id, date, &id_on_date);

    /*** Validate field and value ***/
    if( !strcmp(field, "date") ){ 
    }
    else if( !strcmp(field, "time") ){
    }
    else if( !strcmp(field, "duration"){
    }
    else if( !strcmp(field, "name") || !strcmp(field, "description") || !strcmp(field, "location") ){
    }
    else
        return "field is invalid (must be one of: date, time, duration, name, description, location)";

    /*** Perform update ***/
    int rc = json_object_update_existing_new( // will only succeed if field already existed in event (i.e., is a valid field)
        json_array_get(
            json_object_get(json_object_get(data, calendar_name), date),
            id_on_date
        ),
        field,
        json_string(value)
    );
    if( rc < 0 ) return "no such event exists on this calendar" 
    return NULL;
}

char *get_events_on_date(json_t **results_out, json_t *data, char *calendar_name, char *date){
    if( !validate_date(date, NULL, NULL, NULL) ){
        return "date invalid (expected MMDDYY)";
    }

    // Getting all events on a date is very easy thanks to our data structure
    json_t *events_on_date = json_object_get(json_object_get(data, calendar_name), date);
    // If calendar does not exist (i.e., has no events) or this date does not exist in the calendar (i.e., has no events), return an empty array (signifies no events)
    if( !events_on_date ) events_on_date = json_array();
    
    if( results_out ){
        // However, to account for our 'removed' field, we must filter out removed events before returning the results
        json_t *results = json_array();
        json_t *true_obj = json_true();
        json_array_foreach(events_on_date, index, value){
            if( !json_equal(json_object_get(value, "removed"), true_obj) ){
                json_array_append(results, value);
            }
        }
        json_decref(true_obj);

        // Wrap in an object to tack on numevents field
        json_t *to_return = json_object();
        json_object_set_new("numevents", json_integer(json_array_size(results))); // hand over reference
        json_object_set_new("events", results); // hand over reference

        *results_out = to_return; // caller will need to decref to free everything
    }

    return NULL;
}


char *get_events_in_range(json_t **results_out, json_t *data, char *calendar_name, chra *start_date, char *end_date){
    // Qs:
    //  - start/stop inclusive or exclusive?
    //  - what is output format (are all events contained in one object? are events nested into objects depending on the day?)? it includes a numdays field? why? Should it be a map of date to a list of events? Or just an array of arrays of events, where the caller can determine which fall on which day based on order and start date
    int month, day, year;
    if( !validate_date(start_date, &month, &day, &year) ){
        return "start date invalid (expected MMDDYY)";
    }
    int end_month, end_day, end_year;
    if( !validate_date(end_date, &end_month, &end_day, &end_year) ){
        return "end date invalid (expected MMDDYY)";
    }
    bool start_after_end = (end_year < year) || (end_year == year && end_month < month) || (end_year == year && end_month == month && end_day < day);
    if( start_after_end ){
        return "end date precedes start date (expected end date to follow start date)";
    }

    if( results_out ){
        json_t *to_return = json_object();
        int num_days = 0;
        // TODO: this setup currently uses an inclusive start and exclsuive stop (as is a common paradigm) - is this ok?
        for(; !(day == end_day && month == end_month && year == end_year); num_days++ ){
            char date[7];
            sprintf(date, "%2d%2d%2d", day, month, year);
            json_t *events_today = get_events_on_date(data, calendar_name, date);
            json_object_set_new(to_return, date, events_today); // hand reference to to_return

            // Advance to next day, ignoring leap years
            day = (day % MONTH_TO_N_DAYS[month]) + 1;
            bool is_new_month = day == 1;
            if( is_new_month ) month = (month % 12) + 1;
            bool is_new_year = is_new_month && month = 1;
            if( is_new_year ) year = (year % 99) + 1; 
        }
        json_object_set_new(to_return, "numdays", json_integer(num_days)); // hand reference over
        *results_out = to_return; // caller will need to decref to free everything
    }
    return NULL;
}

json_t *dispatch(json_t *request){
    json_t *result = json_object();
    char *command;
    // TODO: do these error payloads need to include XXXX identifier?
    if( !(command := json_string_value(json_object_get(request, "command"))) ){
        // BAD REQUEST: NO COMMAND
        json_object_set_new(result, "success", json_false());
        json_object_set_new(result, "error", json_string("Request payload is missing 'command' field"));
        return result;
    }
    json_object_set("command", json_object_get(request, "command")); // points to same command json_string as request.command

    char *calendar_name;
    if( !(calendar_name := json_string_value(json_object_get(request, "calendar"))) ){
        // BAD REQUEST: NO CALENDAR
        json_object_set_new(result, "success", json_false());
        json_object_set_new(result, "error", json_string("Request payload is missing 'calendar' field"));
        return result;
    }
    json_object_set("calendar", json_object_get(request, "calendar")); // points to same command json_string as request.calendar

    if( !strcmp(command, "add") ){
        char *calendar_name, *date, *time, *duration_min, *name, *description = NULL, *location = NULL;
        int rc = json_unpack("{"
                "s:s,"
                "s:s,"
                "s:s,"
                "s:s,"
                "s:s,"
                "s?:s,"
                "s?:s"
            "}",
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
            data,
            calendar_name,
            date,
            time,
            duration_min,
            name,
            description,
            location
        );
        // Finish constructing result
        json_object_set_new(result, "success", json_bool(!error));
        if( error ){
            json_object_set_new(result, "error", json_string(error));
            json_object_set_new(result, "identifier", json_string("XXXX"));
        }
        else {
            json_object_set_new(result, "identifier", json_string(event_id));
        }
        return result;
    }
    if( !strcmp(command, "remove") ){
        char *calendar_name, *event_id;
        int rc = json_unpack("{
                s:s,
                s:s,
            }",
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
            data,
            calendar_name,
            event_id
        )
        // Finish constructing result
        json_object_set_new(result, "success", json_bool(!error));
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
        int rc = json_unpack("{
                s:s,
                s:s,
                s:s,
                s:s
            }",
            "calendar", &calendar_name,
            "event_id", &event_id
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
            data,
            calendar_name,
            event_id,
            field,
            value
        )
        // Finish constructing result
        json_object_set_new(result, "success", json_bool(!error));
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
        int rc = json_unpack("{
                s:s,
                s:s,
                s:s
            }",
            "calendar", &calendar_name,
            "event_id", &event_id
            "date", &date
        );
        if( rc < 0 ){
            // BAD REQUEST: WRONG FORMAT
            json_object_set_new(result, "success", json_false());
            json_object_set_new(result, "error", json_string("Invalid request format: expected keys of calendar, event_id, and date, each with a string value."));
            //json_object_set_new(result, "identifier", json_string("XXXX"));
            return result;
        }
        json_t *events_on_date;
        char *error = get_events_on_date(
            &events_on_date,
            data,
            calendar_name,
            date
        )
        // Finish constructing result
        json_object_set_new(result, "success", json_bool(!error));
        if( error ){
            json_object_set_new(result, "error", json_string(error));
            //json_object_set_new(result, "identifier", json_string("XXXX"));
        }
        else {
            json_object_set_new(result, "data", events_on_date); // give ownership of queried data events_on_date to result
        }
        return result; 
        // remember to free events_on_date at some point (after encoding results onto wire)
    }
    if( !strcmp(command, "getrange") ){
        char *calendar_name, *start_date, end_date;
        int rc = json_unpack("{
                s:s,
                s:s,
                s:s
            }",
            "calendar", &calendar_name,
            "event_id", &event_id
            "start_date", &start_date,
            "end_date", &end_date
        );
        if( rc < 0 ){
            // BAD REQUEST: WRONG FORMAT
            json_object_set_new(result, "success", json_false());
            json_object_set_new(result, "error", json_string("Invalid request format: expected keys of calendar, event_id, start_date, and end_date, each with a string value."));
            //json_object_set_new(result, "identifier", json_string("XXXX"));
            return result;
        }
        json_t *events_on_dates;
        char *error = get_events_in_range(
            &events_on_dates,
            data,
            calendar_name,
            start_date,
            end_date
        )
        // Finish constructing result
        json_object_set_new(result, "success", json_bool(!error));
        if( error ){
            json_object_set_new(result, "error", json_string(error));
            //json_object_set_new(result, "identifier", json_string("XXXX"));
        }
        else {
            json_object_set_new(result, "data", events_on_dates); // give ownership of queried data events_on_dates to result
        }
        return result; 
        // remember to free events_on_date at some point
    }
}
    

bool validate_date( char* date, int *month_out, int *day_out, int *year_out ){
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
    // any year is valid

    if( month_out ) *month_out = month;
    if( day_out ) *day_out = day;
    if( year_out ) *year_out = year;
    return true;
}

bool validate_time( char *time, int *hour_out, int *minute_out ){
    if( strlen(time) != 4 ){
        return false;
    }
    char hour_str[3], minute_str[3]; 
    sscanf(date, "%2s%2s", hour_str, minute_str); // sscanf adds NULs to parsed strings
    int hour = atoi(hour_str);
    int minute = atoi(minute_str);

    if( hour <= 0 || hour > 23 ) return false;
    if( minute <= 0 || minute > 59 ) return false;

    if( hour_out ) *hour_our = hour;
    if( minute_out ) *minute_out = minute;
    return true;
}


int main(int argc, char* argv[])
{
    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    struct sigaction sa;
    int yes=1;
    char s[INET6_ADDRSTRLEN];
    int rv;

    // TODO: implement multithreading, probably requiring a lock
    if( argc > 2 || (argc == 2 && strcmp(argv[1], "-mt")) ){
        fprintf(stderr, "[Error] Usage: %s [-mt]", argv[0]);
    }

    char *port = get_port_from_cfg(CONFIG_PATH);

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

    /* Might be able to remove SIGCHD handler if threading instead of forking */
    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("[Error] Failed to set sigaction");
        exit(1);
    }
    /* --- */
    // TODO: register ctrl-C handler to save data

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

        // TODO: refactor to multi-thread, not fork
        if (!fork()) { // this is the child process
            close(sockfd); // child doesn't need the listener
            int rc = client_handler(new_fd);
            close(new_fd);
            exit(rc);
        }
        close(new_fd);  // parent doesn't need this
    }

    return 0;
}

json_t *get_next_request(int sockfd){
    static char *buffer = malloc(BUFSIZ);
    static int space_remaining = BUFSIZ; // essentially tracks end of meaningful data in buffer
    int current_bufsiz_multiple = 1; // resets to 1 on every invocation
    do {
        // If contains a newline, this request fits in the current buffer.
        // Parse request and save reference in buffer so we can pick up from here next time (in case we received 2 full requests at once)
        char *newline = strnchr(buffer, current_bufsiz_multiple * BUFSIZ - space_remaining, '\n');
        if( newline ){
            // Parse JSON up to newline
            // Parse now so we can throw out the string we parsed from: returning a string might be more general, but would require allocating space for the copied request so we can free the buffer (certainly doable but adds complexity which is uneeded here).
            json_t *to_return = json_loadb(buffer, newline - buffer, 0, NULL);

            // Bookkeep for next time
            // Because we check after every BUFSIZ-sized chunk, we can be sure that the data beyond the newline will fit into a BUFSIZ-size buffer
            int n_bytes_to_preserve = current_multiple * BUFSIZ - (newline - buffer) - 1; // left in buffer beyond newline: all - parsed - 1: < BUFSIZ
            char *small_buffer = malloc(BUFSIZ);
            strncpy(small_buffer, newline + 1, n_bytes_to_preserve);
            free(buffer); // release space after each request: if we keep adding onto same buffer we will consume lots of memory we aren't using
            buffer = small_buffer;
            space_remaining = BUFSIZ - n_bytes_to_preserve; // everything beyond what we will preserve is free buffer space
            return to_return;
        }

        // If buffer full, allocate more space: keep it contiguous so we can JSON parse it all together
        if( space_remaining == 0 ){
            current_bufsiz_multiple += 1;
            buffer = realloc(current_bufsiz_multiple * BUFSIZ);
        }

        // Receive more data to try to find end of request
        int got = recv(sockfd, buffer + (current_bufsiz_multiple * BUFSIZ - space_remaining), space_remaining);
        // TODO: ERRCHECK
        space_remaining -= got;
    } while(true);
}

int client_handler(int sockfd){
    // TODO: implement
    // while true:
    //  get_next_request
    //  dispatch
    //  encode json result and send to client
}
