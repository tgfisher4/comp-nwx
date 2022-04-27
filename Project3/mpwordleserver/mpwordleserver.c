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


/* Data Types */
// Note: We generally tried to use json_t for data structures as needed since we already have them, and they are very flexible and cover most use cases.
//       However, json_t has no way to store functions (understandably), so for data structures requiring function pointers, needed to use plain ol' structs.
typedef json_t *(*message_handler_t)(json_t *message, int sockfd, json_t **broadcast_messages_out);

struct handle_socket_thread_wrapper_arg{
    bool is_lobby;
    int sockfd;
    message_handler_t handler;
};

typedef json_t *(*data_resolver_t)(char *player_name);

struct broadcast_message {
    // The message to broadcast
    json_t *message;
    // For some messages, we desire that the value of certain fields is different for each player: thus the value must be resolved dynamically outside the broadcast_message specification
    // A dynamic field specifies a field within message.data to resolve dynamically, and a function that can resolve it when turning this spec into messages to send to each client
    // This is a linked list of pointers to dynamic field specs (NULL indicates end of list)
    struct dynamic_field **data_fields_to_resolve;
}

// Takes the message and a NULL-terminated list of pointers to dynamic fields to embed into the broadcast
// Note: this function *steals* the jsont_t reference to message (in the jansson sense). This is assumed to be convenient for the caller in the majority of cases.
// Note: this function *steals* dynamic_fields (created via dynamic_field_create) in the sense that it will call dynamic_field_destroy on them when it is destroyed: the caller should NOT manually invoke destroy these dynamic_fields.
// That is, it allows the dynamic_fields passed to live only as long as it does (this is assumed to be a service to the caller so they don't have to maintain handles to the dynamic fields).
struct broadcast_message *broadcast_message_create(json_t *message, ...){
    struct broadcast_message *to_return = malloc(sizeof(broadcast_message));
    // TODO: am I fail-checking mallocs? very unlikely and not really any recourse

    // Note: NOT incref'ing the message effectively steals the reference: otherwise, we would want to incref since the caller has a reference and we are saving another.
    to_return->message = message;

    // On the first pass, just count the number of dynamic fields.
    va_list args;
    va_start(args, message);
    int n_dynamic_fields = 1; // arg list will be NULL terminated, so this count includes the NULL arg
    while( va_arg(args, dynamic_field *) ){ // i.e., while the next field is not NULL
        ++n_dynamic_fields;
    }
    va_end(args);

    // Even if no dynamic fields, dynamic_field will itself be a non-null double pointer pointing to a NULL single pointer.
    // In particular, this means we always need to allocate SOME space (at least one pointer's worth).
    // This creation function nicely encapsulates this subtlety.
    to_return->dynamic_fields = malloc(n_dynamic_fields * sizeof(dynamic_field *));
    va_start(args, message);
    int idx = 0;
    struct dynamic_field *field;
    while( field = va_arg(args, dynamic_field *) ){
        to_return->dynamic_fields[idx] = field; // steal reference
        ++idx;
    }
    va_end(args);

    return to_return;
}

void broadcast_message_destroy(struct broadcast_message *msg){
    json_decref(msg->message);
    for( int field_idx = 0; msg->dynamic_fields[field_idx]; ++field_idx ){
        dynamic_field_destroy(msg->dynamic_fields[field_idx]);
    }
    free(msg->dynamic_fields);
    free(msg);
}

// TODO: does this need to be before broadcast_message?
struct dynamic_field {
    char *name;
    data_resolver_t resolver;
}

// Note: does NOT steal the string name, but duplicates it (to allow for non-malloc'd strings)
struct dynamic_field *dynamic_field_create(char *name, data_resolver_t resolver){
    struct dynamic_field *field = malloc(sizeof(struct dynamic_field));
    field->name = strdup(name);
    field->resolver = resolver;
}

void dynamic_field_destroy(struct dynamic_field *field){
    free(field->name);
    free(field);
}

/* Globals */
json_t *player_to_info;
json_t *round_info;
pthread_mutex_t *global_lock = PTHREAD_MUTEX_INITIALIZER;
int game_accept_socket;

// Default parameter values
int N_PLAYERS_PER_GAME = 2;
int LOBBY_PORT = 4100;
int GAME_PORT_START = 4101;
int N_ROUNDS = 3;
FILE *DICT_FILENAME = NULL;
bool DEBUG = false;
int N_GUESSES = 6; // not configurable

/* Prototypes */
json_t *get_next_request(int sockfd);
/*
char *get_port_from_cfg(char *cfg_pathname);
int mkpath(char *dir_path, mode_t mode);
json_t *load_data(char *data_path);
void save_data(char *data_path, json_t *database, pthread_rwlock_t lock, int *exit_status);
void *run_client_thread(void *void_args);
void handle_client(int sockfd, json_t *database, pthread_rwlock_t lock);
json_t *dispatch(json_t *request, json_t *database, pthread_rwlock_t lock);
char *add_event(char **event_id_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *date, const char *time, const char *duration_min, const char *name, const char *description, const char *location);
char *remove_event(json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *event_id);
char *update_event(json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *event_id, const char* field, const char *value);
char *get_events_on_date(json_t **results_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *date);
char *get_events_in_range(json_t **results_out, json_t *database, pthread_rwlock_t lock, const char *calendar_name, const char *start_date, const char *end_date);
*/

/* Idea:    Separate server thread for each client to listen to messages or guesses asynchronously.
 *          Obv lock when dealing with shared data and to make sure the last person in triggers a response to all.
 *          Client should have separate thread to listen to async server messages to record chat messages or begin next guess/round
 *
 *          Global lock upon receiving a message to forward to other players has the advantage of providing a universal serialization point for all messages
 *              - since we are sending over a TCP stream, there should be no worry of reordering in-flight: if 'send's succeed, they will be recv'd in that order
 */

/* Functions */
int main(int argc, char *argv[]){

    // Process args
    if( !process_args(argc, argv) ){
        usage(1);
    }

    // Setup
    srand(NULL);
    signal(SIGCHLD, SIG_IGN);
    player_to_info = json_object();
    round_info = json_object();

    // Launch lobby
    start_server(LOBBY_PORT, 0, lobby_accept_handler);
}

void usage(char *invoked_as, int exit_code){
    printf(
        "Usage: %s [options]\n"
        "Options:
        "   -np X: Each game includes X players. The default is 2.\n"
        "   -lp X: The main lobby server listens on port X. The default is 4100.\n"
        "   -pp X: The starting play/game port is X. When the lobby capacity is reached, a new game begins on another port, the first being X and incrementing for future games (as availability allows). The default is 4101.\n"
        "   -nr X: Each game includes X rounds. The default is 3.\n"
        "   -d dictname: Words will be drawn from the file at path 'dictfile'. List each word on a separate line. Words must be between 3 and 10 letters, inclusive. The default dictionary is one we have constructed.\n"
        "   -dbg: The servers will print debugging information whenever they receive or send a message (it does not by default).\n"
        , invoked_as
    );
    exit(exit_code);
}

/* Processes command line args, setting globals as appropriate, and returns whether the processing was successful */
bool process_args(int argc, char **argv){
    struct option n_players_opt = {"np", required_argument, NULL, 0};
    struct option lobby_port_opt = {"lp", required_argument, NULL, 1};
    struct option game_port_start_opt = {"pp", required_argument, NULL, 2};
    struct option n_rounds_opt = {"nr", required_argument, NULL, 3};
    struct option dict_filename_opt = {"d", required_argument, NULL, 4};
    struct option debug_on_opt {"dbg", no_argument, NULL, 5};

    struct option longopts[7];
    longopts[0] = n_players_opt;
    longopts[1] = lobby_port_opt;
    longopts[2] = game_port_start_opt;
    longopts[3] = n_rounds_opt;
    longopts[4] = dict_filename_opt;
    longopts[5] = debug_on_opt;
    longopts[6] = {0, 0, 0, 0}; // sentinel

    int val, idx;
    while( (val = getopt_long_only(argc, argv, "", longopts, &idx)) > -1 ){
        switch(val){
            case 0:
                if( !str_to_int(optarg, &N_PLAYERS_PER_GAME) ){
                    fprintf(stderr, "option '-np' expects an integer argument");
                    return false;
                }
                break;
            case 1:
                if( !str_to_int(optarg, &LOBBY_PORT) ){
                    fprintf(stderr, "option '-lp' expects an integer argument");
                    return false;
                }
                break;
            case 2:
                if( !str_to_int(optarg, &GAME_PORT_START) ){
                    fprintf(stderr, "option '-pp' expects an integer argument");
                    return false;
                }
                break;
            case 3:
                if( !str_to_int(optarg, &N_ROUNDS) ){
                    fprintf(stderr, "option '-nr' expects an integer argument");
                    return false;
                }
                break;
            case 4:
                DICT_FILE = open(optarg, O_RDONLY);
                if( !DICT_FILE ){
                    perror("couldn't open dictionary file");
                    return false;
                }
                line_err = validate_diciontary(DICT_FILE);
                if( line_err ){
                    fprintf(stderr, "All words in the dictionary file must be 3-10 letters, inclusive: line %d of %s does not comply", line_err, optarg);
                    return false;
                }
                break;
            case 5:
                DEBUG = true;
                break;
            default:
                printf("[INFO] Somehow reached default in process_args switch (val was %d)\n", val);
                return false;
                break; // impossible
        }
    }
    if( optind != argc ){
        fprintf(stderr, "unrecognized option %s", argv[optind]);
        return false;
    }
    return true;
}

int validate_dictionary(FILE *dict){
    size_t lineno = 1;
    char current[12]; // Max size we need is 12: 10 chars, 1 newline, 1 NUL

    fseek(DICT_FILE, 0, SEEK_SET);
    while( fgets(current, sizeof(current), DICT_FILE) ){
        // No newline means larger than 11 chars: invalid
        // Length of 5 or smaller means 3 or fewer non-newline chars: invalid
        if( !strrchr(current, '\n') || strlen(current) <= 3 ){
             return lineno;
        }
        ++lineno;
    }
    return 0;
}

int bind_server_socket(char *port){
    int sockfd;
    int yes=1;
    int rv;
    struct addrinfo hints, *servinfo, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    // Get info on own addr
    if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "[Error] Failed to getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // Loop through all the results and bind to the first we can
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

    if (p == NULL)  {
        fprintf(stderr, "[Error] Failed to bind any socket");
        exit(1);
    }

    return sockfd;
}

/* Transform this process into a server */
void start_server(char *port, int bound_server_socket, void (*accept_handler)(int sockfd)){//, message_handler){
    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    //struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    //int yes=1;
    char s[INET6_ADDRSTRLEN];
    //int rv;

    sockfd = bound_server_socket ? bound_server_socket : bind_server_socket(port);

    /*
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    // Get info on own addr
    if ((rv = getaddrinfo(NULL, port, &hints, &servinfo)) != 0) {
        fprintf(stderr, "[Error] Failed to getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // Loop through all the results and bind to the first we can
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

    if (p == NULL)  {
        fprintf(stderr, "[Error] Failed to bind any socket");
        exit(1);
    }
    */

    if (listen(sockfd, BACKLOG) == -1) {
        perror("[Error] Failed to listen");
        exit(1);
    }

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

        accept_handler(new_fd);
    }
}


// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


void generic_accept_handler(bool is_lobby, int sockfd, message_handler_t handler){
    // Note: this allocation will be freed at the end of the thread rountine.
    struct handle_socket_thread_wrapper_arg *arg = malloc(sizeof struct handle_socket_thread_wrapper_arg);
    // TODO: err check?
    arg->is_lobby = is_lobby;
    arg->sockfd = sockfd;
    arg->handler = handler;
    pthread_t thd;
    pthread_create(&thd, NULL, handle_socket_thread_wrapper, arg);
    // Since we don't intend to join the thread, we detatch it immediately
    pthread_detatch(thd);
}

void *handle_socket_thread_wrapper(void *arg){
    struct handle_socket_thread_wrapper_arg *typed_arg = (*handle_socket_thread_wrapper_arg) arg;
    handle_socket(typed_arg->is_lobby, typed_arg->sockfd, typed_arg->handle_message);
    free(typed_arg);
    return NULL;
}

void handle_socket(bool is_lobby, int sockfd, message_handler_t handle_message){ // data is global so it can be manipulated by all (could also accept a pointer to it)
    while(true){
        json_t *request = get_next_request(sockfd);
        
        // NOTE: after much thinking about concurrency, I think select/poll makes actually more sense for this project.
        // Consider the following scenario in a 2-player game
        //  1) Player 1 submits guess: p1 thread processes it without issue.
        //  2) Player 2 submits guess: p2 locks, records guess in global, unlocks
        //  3) Player 1 submits unsolicited guess (assuming the worst): suppose p1 thread then grabs the lock (BEFORE p2 can grab it to run start_next_round), sees p1 and p2 have same number of guesses, assesses p1's guess against the WRONG reference word - the next word to be chosen by start_next_round may have a different number of letters.
        // This and other cases (involving when to evaluate whether a round is over, when to evaluate the next guess number) show that, in general, my critical sections are pretty much the ENTIRE processing of a response: perhaps I could use more complex global variables with my lock to try to keep threads from interfering with one another, but the goal of this work would pretty much be to serialize 'Guess' messages.
        // But then, when some functions are expecting the caller to hold the lock and some are not, things get messier.
        // At the very least, it is simplest to serialize ALL messages, and with very small games such as these this should not incur any measurable performance penalty
        // Of course, the best way to do this would be to use select/poll, where all requests are serialized and processed in order (no concurrency).
        // Since I already started pretty far down the thread route, this implementation will use the global lock to essentially re-implement select/poll since refactoring is not feasible.
        pthread_mutex_lock(&global_lock);

        if( !request ){
            printf("[Info] Connection with client (via socket %d) broken: unable to recv.\n", sockfd);
            break;
        }
        json_t *result;
        json_t **broadcast_messages;
        if( json_is_string(request) ){ //  if it's a string, it's an error message
            result = json_object();
            json_object_set_new(result, "MessageType", json_string("InvalidRequest"));
            // Set calls incref on value (respecting the caller's reference) so this should be correct.
            json_object_set(result, "Error", request);// json_copy(request));
            //json_object_set_new(result, "Error", json_copy(request));
        }
        else{
            result = handle_message(request, sockfd, &broadcast_messages);//, database, lock); // maybe?
        }

        if( result ){
            if( !dump_json(sockfd, result) ){
                printf("[Info] Connection with client (via socket %d) broken: unable to send.\n");
                break; // send failed, close cxn
            }
        }

        json_t *message, *info;
        const char *player_name;
        for( int msg_idx = 0; broadcasts[msg_idx]; ++msg_idx ){
            struct broadcast_message *msg = broadcasts[msg_idx];
            json_object_foreach(player_to_info, player_name, info){
                for( int field_idx = 0; msg->dynamic_fields[field_idx]; ++field_idx ){
                    struct dynamic_field *field = msg->dynamic_fields[field_idx];
                    json_object_set_new(json_object_get(msg->message, "Data"), field->name, json_string(field->resolver(player_name)));
                }
                if( !dump_json(json_integer_value(json_object_get(info, "sockfd")), message) ) ;// TODO: what if one of these fails? Ignore, right? Let the 'owning' socket handle it?
            }
            broadcast_message_destroy(msg);
        }
        json_decref(request);
        json_decref(result);

        // Determine if major change needed
        int n_broadcasts = 0;
        for( ; broadcasts[n_broadcasts]; ++n_broadcasts ) ; // advance n_broadcasts until we hit NULL
        bool was_broadcast_sent = n_broadcasts > 0;

        bool was_end_game_sent = was_broadcast_sent && json_string_value(json_object_get(broadcasts[n_broadcasts - 1]->message, "MessageType")) == "EndGame";
        bool was_start_instance_sent = was_broadcast_sent && json_string_value(json_object_get(broadcasts[n_broadcasts - 1]->message, "MessageType")) == "StartInstance";

        // Free broadcasts
        for( int msg_idx = 0; broadcasts[msg_idx]; ++msg_idx ){
            broadcast_message_destroy(msg);
        }
        free(broadcast_messages);

        // TODO: I think this never actually closes the original socket of the last person to join
        //  - since the game server is short-lived and this is a one-time occurence, there won't be any leaking of memory: should be fine to leave it as is
        if( was_start_instance_sent ) transition_to_game();
        if( was_end_game_sent ) exit(1); // trust OS to free memory, close sockets, etc

        pthread_mutex_unlock(&global_lock);
    }

    // When closing socket, we may want to delete a player's info.
    // This both decrements the number of players in the lobby and frees unused space.
    //  - in lobby, want to delete joined player info when they leave: need an additional player to start
    //  - when transitioning from lobby to game, we want to close old sockets but do not want to delete player info
    //  - if socket closed but client didn't actually join game (e.g. sent gibberish), nothing to delete bc we never added
    //  Solution:
    //      - if game full, players are locked in (do not delete)
    //      - if game not full, when a player leaves check if they had joined (if sockfd present in player_to_info data structure)
    //          - this is slightly annoying because player_to_info is set up to be queried by player_name, not sockfd
    //          - however, since there will likely only ever be a handful of players (and we don't expect players to leave often), a linear search is bearable
    //  Note: There is no problem when clients in the lobby are leaving (one might expect they do not free their info bc lobby seems full) bc info is freed in handle_join
    const char *player_name;
    json_t *player_info;
    void *tmp;
    //  Note: We are sure to lock in case another thread is adding a player to player_to_info (prevent del-check | add add-check start-game | del-info)
    if( json_object_size(player_to_info) != n_players_per_game ){
        json_object_foreach_safe(player_to_info, tmp, player_name, player_info){ // use 'safe' version since we call delete within loop
            if( json_integer_value(json_object_get(player_info, "sockfd")) == sockfd ){
                json_object_del(player_to_info, player_name);
                break;
            }
        }
    }
    // TODO: extension - if game server, mark client as disconnected
    pthread_mutex_unlock(&global_lock);

    close(sockfd);
    // Receive next message
    // If no next message, close socket and return
    // If client close connection while waiting, seems we'd want to discard info from our bookkeeping records: if someone else has already submitted last join and we've grabbed the lock for that, then we're in trouble bc we'll start the new game but never have enough players because clt has left/won't receive game port
    // Handler processes message
    // Lock
    // If handler gives an individual response, send back to same socket
    // If handler gives a broadcast response, send to each socket
    // Unlock
    // Loop back
}

bool dump_json(int sockfd, json_t *json){
    if( json_dumpfd(result, sockfd, JSON_COMPACT) < 0 || send(sockfd, "\n", 1, 0) != 1 ){ // takes care of sending over TCP (streaming) socket
        fprintf(stderr, "[Error] Problem sending result back to client. Aborting connection...\n");
        return false;
    }
    return true;
}

void lobby_accept_handler(int sockfd){
    generic_accept_handler(true, sockfd, lobby_message_handler);
    // Create new thread to listen to newly accepted socket and handle messages
    // Add to global array
    //  - actually, I now think adding to bookkeeping structures should be handled by MessageType handlers themselves, in case the message is junk and we don't actually wnt to add a new the client to our state

    // if capacity reached, fork and tell clients new address
    //   manage through static variable? pros/cons vs global?
    //   also, could just count total number of clients accepted and mod/div by capacity to determine port number (game_port_start + n_clts_accepted/capacity)
    // create a new thread to listen to this client's socket
    //   should this just be the default behavior in start_server, i.e., to spawn a listening thread with a specific message handler?
    //   seems like most general would be an accept handler and the message_handler will be built into the accept handler
    //   since this isn't python, we can't return a function. But maybe we can be polymorphic in other ways.
    //
    // message handler: just forward chats to all players/skts
}


// I need to "return" a double pointer, so they need to pass in a triple pointer
jsont_t *lobby_message_handler(json_t *message, int sockfd, struct broadcast_message ***broadcasts){
    char *err = NULL;
    *broadcasts = malloc(sizeof(struct *broadcast_message));
    **broadcasts = NULL;

    json_t *response = json_object();
    json_object_set_new(response, "Data", json_object());

    const char *message_type = json_string_value(json_object_get(message, "MessageType"));
    if( !message_type ){
        err = "Required string field '.MessageType' is missing or is not a string";
        goto return_error;
    }
    json_t *message_data = json_object_get(message, "Data");
    if( !message_data ){
        err = "Required field '.Data' is missing";
        goto return_error;
    }

    if( !strcmp(message_type, "Chat") ){
        json_object_set_new(response, "MessageType", json_string("ChatResult"));

        // Should we send an error message to the client if they don't send a valid chat?
        char *player_name = json_object_get(message_data, "Name");
        if( !player_name ){
            err = "Expected string field '.Data.Name' is missing or is not a string";
            goto return_error;
        }
        char *text = json_string_value(json_object_get(message_data, "Text"));
        if( !text ){
            err = "Expected string field '.Data.Text' is missing or is not a string";
            goto return_error;
        }
        char *text_out;
        err = handle_chat(player_name, sockfd, text, &text_out);
        if( err ) goto return_error;

        // Copy the message since broadcast_message_create we want to (a) steal the reference, (b) potentially modify the chat
        struct broadcast_message *chat_broadcast = broadcast_message_create(json_copy(message), NULL);
        json_object_set_new(json_object_get(chat_broadcast->message, "Data"), "Text", json_string(text_out));
        free(text_out);

        free(*broadcasts); // free the placeholder
        *broadcasts = malloc(2 * sizeof(broadcast_message *));
        (*broadcasts)[0] = chat_broadcast;
        (*broadcasts)[1] = NULL;

        json_decref(response); // chat sends no personal response to the client
        return NULL;
    }
    
    if( !strcmp(message_type, "Join") ){
        json_object_set_new(response, "MessageType", json_string("JoinResult"));
        // We expect failure by default so we'll be ready to jump to return_error without having to set this up in each case.
        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("No"));

        char *player_name = json_string_value(json_object_get(message_data, "Name"));
        if( !player_name ){
            err = "Expected string field '.Data.Name' is missing or is not a string";
            goto return_error;
        }
        json_object_set_new(response_data, "Name", json_string(player_name)); // go ahead and make new ref bc I'm lazy and it's just one short-lived string

        char *client_name = json_string_value(json_object_get(message_data, "Client"));
        if( !client_name ){
            err = "Expected string field '.Data.Client' is missing or is not a string";
            goto return_error;
        }

        int port;
        err = handle_join(player_name, sockfd, &port);
        if( err ) goto return_error;


        // Distinguish cases with 'port': essentially a return code that also indicates new port if positive and indicates other things if negative
        //  - TODO: is return code a better name?
        //  - TODO: where to ensure game port is valid (if listening fails, choose a different port, probably incrementing until we're good)
        
        // Must ensure only one of the parent server and child server processes that return from handle_join sends responses to clients
        //  - philosophy: child server serves as the LOBBY for all joined clients for a short while while it prepares a startinstance message
        //                i.e., the child server will respond to clients when enough have joined the lobby
        //      - since a child process inherits sockets it can also send from the same port (socket represents 5-tuple) so clients will recognize it

        if( port < 0 ){ // new gameInstance started: lobby server should not pass through to send response message (new game server will handle)
            json_decref(response);
            return NULL;
        }
        // port == 0 ==> not enough players yet: lobby server will not construct broadcast message but will pass through to send response message
        //  - note that upon error, we have already jumped to return_error
        if( port ){ // new game server: construct broadcast message and pass through to send response message
            json_t *start_instance_data = json_object();
            
            char my_host[BUFSIZ];
            get_hostname(my_host, BUFSIZ);
            // TODO: err check?
            json_object_set_new(start_instance_data, "Server", json_string(my_host)); // fine (in fact, better) to do this on stack
            json_object_set_new(start_instance_data, "Port", json_integer(port));
            json_object_set_new(start_instance_data, "Nonce", json_null());

            json_t *start_instance_message = json_object();
            json_object_set_new(start_instance_message, "MessageType", json_string("StartInstance"));
            json_object_set_new(start_instance_message, "Data", start_instance_data);

            struct broadcast_message *start_instance_bcast = broadcast_message_create(start_instance_message, dynamic_field_create("Nonce", resolve_nonce), NULL);

            free(*broadcasts); // free placeholder
            *broadcasts = malloc(2 * sizeof(struct broadcast_message *));
            (*broadcasts)[0] = start_instance_bcast;
            (*broadcasts)[1] = NULL; // null-terminate the array
        }
        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("Yes")); // does replacing a value decref the old one?
        return response;    
    }
    else {
        err = "Unrecognized MessageType. Must be one of: 'Join', 'Chat'";
        json_object_set_new(response, "MessageType", json_string("InvalidRequest"));
        goto return_error;
    }

        return_error: /* LABEL */
    // TODO: Note in readme that we will include an '.Error' field in our 'Response' message detailing the error encountered if one occurs during processing
    json_object_set_new(response, "Error", json_string(err));
    return response;


    // if Join
    //   individual response = name taken ? reject : accept
    //   if we will accept player into lobby and thus reach capacity, compute broadcast response (StartInstance)
    //   else, broadcast result is nothing
    // if Chat, return message to broadcast and NULL to respond
    // else, error? send back invalid MessageType or smth of sort?
}

char *resolve_info_field(char *field, char *player_name){
    return json_string_value(json_object_get(json_object_get(player_to_info, player_name), field));
}

char *resolve_nonce(char *player_name){
    return resolve_info_field("nonce", player_name);
}

char *handle_join(char *player_name, int sockfd, int *port_out){
    *port_out = 0; // default return value (in case of error or player join but not enough for game)

    // If name already taken, return an error
    bool is_name_yours = !verify_name(player_name, sockfd);
    if( name_is_yours ){
        return "You have already joined the game lobby";
    }
    bool is_name_taken = !strcmp(player_name, "mpwordle") || json_object_get(player_to_info, player_name);
    if( name_is_taken ){
        return "That name is taken";
    }

    json_t *player_info = json_object();
    json_object_set_new(player_info, "sockfd", json_integer(sockfd));
    json_object_set_new(player_info, "nonce", json_integer(rand() % 1000000)); // TODO: srand during setup in main

    json_object_set_new(player_to_info, player_name, player_info);
     
    // TODO: use OS-assigned port and report to user - to hell with the start play port
    // static next_game_port = GAME_PORT_START; // TODO where to validate this port will work?
    // If lobby now full, launch new game
    if( json_object_size(player_to_info) == N_PLAYERS_PER_GAME ){
        if( fork() ){ // parent
            // Ignore child, go about business

            /* Close connections to lobby clients */
            //  - use shutdown calls: this will awake threads and allow them to close their sockets and exit: see transition_to_game for long-winded explanation
            //  - making sure previous clients' threads exit is important so superfluous state doesn't build up as server runs over long period
            json_object_foreach(player_to_info, player_name, info){
                //close(json_integer_value(json_object_get(info, "sockfd")));
                shutdown(json_integer_value(json_object_get(info, "sockfd")), SHUT_RDWR);
            }
            
            /* Cleanup player_to_info so that we don't accumulate leak memory as we fork lobbies */
            json_object_clear(player_to_info);
            
            // Note: Interactions with player_to_info-deleting logic in handle_socket are fine
            // E.g.: Lobby locks. Chat message placed in buffers. Lobby forks, returns, and shutsdown each socket and deletes all player info. Lobby unlocks. Thread errs on shutdown socket, locks. Thread sees player_to_info not full (cleared by Lobby), searches player_to_info (nothing there), can't find, doesn't delete anything, unlocks.

            *port_out = -1; // indicate we are parent server but new game has been spawned
        } else { // child: decide game port
            game_accept_socket = bind_server_socket(NULL); // TODO: does this work?
            struct sockaddr_in sa;
            int sa_len = sizeof(sa);
            sa_len = sizeof(sa);
            if (getsockname(s, &sa, &sa_len) == -1) {
                perror("getsockname() failed");
                return -1;
            }
            *port_out = ntohs(sa.sin_port);
        }
    }
}


void transition_to_game(){
    // TODO: maybe, don't need to close accept socket
    //  - we won't use the accept socket further, but this game is not designed to be a long-running process, so it will exit relatively soon and OS will close for us
    // Close accept_socket
    // To get a handle on it all the way here, gonna cheat/hack and use a global
    // Needed a global anyway to pass the accept_socket from handle_join to transition_to_game, so before transition I can use this global
    // close(accept_socket);
           
    // <strikethrough> Close </strikethrough> Shutdown each lobby client socket.
    //   - if indeed a client's chat is received as the last client to hit capacity is joining, we will need to consider the case that we grabbed lock, closed sockets, released lock, chatting clt thd grabs, tries to send, err bc sockets closed
    //   - after some research, it appears the best way to stop our listener threads from using sockets is to shut them down and then allow the listener threads to close them when their recvs fail
    //      - https://stackoverflow.com/questions/3589723/can-a-socket-be-closed-from-another-thread-when-a-send-recv-on-the-same-socket#:~:text=Yes%2C%20it%20is%20ok%20to,will%20report%20a%20suitable%20error.&text=In%20linux%20atleast%20it%20doesn,call%20close%20from%20another%20thread
    //      - one catch: linux doesn't guarantee that blocked recv's will unblock with err/zero data, but most implementations seem to support this
    //  - we don't do this earlier because we need the sockets alive to broadcast the game port
    json_object_foreach(player_to_info, player_name, info){
        shutdown(json_integer_value(json_object_get(info, "sockfd")), SHUT_RDWR);
        // Mark each client as disconnected
    }
    pthread_mutex_unlock(&global_lock);
    // Start new server with slightly different accept handler
    start_server(game_port, game_accept_socket, game_accept_handler);
}

char *handle_chat(char *player_name, int sockfd, char *text, char **text_out){
    char *err = verify_name(player_name, sockfd);
    if( err ) return err;

    *text_out = strdup(text);
    return NULL;
}

char *verify_name(char *player_name, int sockfd){
    return sockfd == json_integer_value(json_object_get(json_object_get(player_to_info, player_name), "sockfd")) ? NULL : return "That's not your name!";
}


/* -------------------- */
/* Game Server Handlers */
/* -------------------- */
void game_accept_handler(int sockfd){
    generic_accept_handler(false, sockfd, game_message_handler);
    // Any security to ensure that only expected clients are joining? The nonce I guess, but is this just send in cleartext?
    // Decision: trigger capacity events when person has joined or when we process their 'join' message?
    //  - I think best practice would def be when we process their 'join' message (could be some rando sending junk - don't want to start game yet)
    // Create new thread to listen to this client's skt
    // Add to global array for broadcasting
    // message handler:
    //  - forward chats
    //  - record guesses from unique players
    //  - if all guesses received, evaluate vs answer and send results to all
}

// game_message_dispatcher better name?
// TODO: similar structure of game_message_handler and lobby_message_handler: is a generalization possible/desirable, taking a specification of functions to invoke for each message_type?
//  - might be cool, but at some point need to get this project done
//  - might be desirable if more, similarly structured handlers were expected, but they are not
jsont_t *game_message_handler(json_t *message, int sockfd, struct broadcast_message ***broadcasts){
    char *err = NULL;
    // Default to no broadcasts
    *broadcasts = malloc(sizeof(struct *broadcast_message));
    **broadcasts = NULL;

    json_t *response = json_object();
    json_object_set_new(response, "Data", json_object());

    const char *message_type = json_string_value(json_object_get(message, "MessageType"));
    if( !message_type ){
        err = "Required string field '.MessageType' is missing or is not a string";
        goto return_error;
    }
    json_t *message_data = json_get(message, "Data");
    if( !message_data ){
        err = "Required field '.Data' is missing";
        goto return_error;
    } 

    // Looping variables everyone in this function can use
    const char *player;
    json_t *info, *tmp;

    if( !strcmp(message_type, "Chat") ){
        json_object_set_new(response, "MessageType", json_string("ChatResult"));

        // Should we send an error message to the client if they don't send a valid chat?
        char *player_name = json_object_get(message_data, "Name");
        if( !player_name ){
            err = "Expected string field '.Data.Name' is missing or is not a string";
            goto return_error;
        }
        char *text = json_string_value(json_object_get(message_data, "Text"));
        if( !text ){
            err = "Expected string field '.Data.Text' is missing or is not a string";
            goto return_error;
        }
        char *text_out;
        err = handle_chat(player_name, sockfd, text, &text_out);
        if( err ) goto return_error;

        // Copy the message since broadcast_message_create we want to (a) steal the reference, (b) potentially modify the chat
        // TODO: check that copying an object increfs all the elements
        struct broadcast_message *chat_broadcast = broadcast_message_create(json_copy(message), NULL);
        json_object_set_new(json_object_get(chat_broadcast->message, "Data"), "Text", json_string(text_out));
        free(text_out);

        free(*broadcasts); // free the placeholder
        *broadcasts = malloc(2 * sizeof(broadcast_message *));
        (*broadcasts)[0] = chat_broadcast;
        (*broadcasts)[1] = NULL;

        json_decref(response); // chat sends no personal response to the client
        return NULL;
    }
    if( !strcmp(message_type, "JoinInstance") ){
        json_object_set_new(response, "MessageType", json_string("JoinInstanceResult"));
        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("No"));

        const char *player_name = json_string_value(json_object_get(message_data, "Name"));
        if( !player_name ){
            err = "Expected string field '.Data.Name' is missing or is not a string";
            goto return_error;
        }
        json_object_set_new(json_object_get(response, "Data"), "Name", json_string(player_name));

        int nonce = json_integer_value(json_object_get(message_data, "Nonce"));
        if( !nonce ){
            err = "Expected integer field '.Data.Nonce' is missing or is not an integer";
            goto return_error;
        }

        bool start_game;// = false;
        err = handle_join_instance(player_name, nonce, sockfd, &start_game);
        if( err ){
            goto return_error;
        }

        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("Yes"));

        if( !start_game ){
            return response;
        }

        // Construct StartGame broadcast
        json_t *start_game_message = json_object();
        json_object_set_new(start_game_message, "MessageType", json_string("StartGame"));

        json_t *start_game_message_data = json_object(); 
        json_object_set_new(start_game_message_data, "Rounds", json_integer(N_ROUNDS));
        json_t *player_info = json_array();
        json_object_foreach(player_to_info, player, info){
            json_t *info_to_append = json_object();
            // Don't steal references, share them
            json_object_set(info_to_append, "Name", json_object_get(info, "Name"));
            json_object_set(info_to_append, "Number", json_object_get(info, "Number"));
            json_array_append_new(player_info, info_to_append);
        }
        json_object_set_new(start_game_message_data, "PlayerInfo", player_info);
        json_object_set_new(start_game_message, "Data", start_game_message_data);

        struct broadcast_message *start_game_broadcast = broadcast_message_create(start_game_message, NULL);
        
        // Construct StartRound broadcast
        struct broadcast_message *start_round_broadcast = compile_start_round_broadcast(); //broadcast_message_create(start_round_message, NULL);
        
        // Construct PromptForGuess broadcast
        struct broadcast_message *prompt_for_guess_broadcast = compile_prompt_for_guess_broadcast();

        // Link together into broadcasts
        free(*broadcasts); // free placeholder
        *broadcasts = malloc(4 * sizeof(struct broadcast_message *));
        (*broadcasts)[0] = start_game_broadcast;
        (*broadcasts)[1] = start_round_broadcast;
        (*broadcasts)[2] = prompt_for_guess_broadcast;
        (*broadcasts)[3] = NULL; // null-terminate the array
        
        return response;
    }
    else if( !strcmp(message_type, "Guess") ){
        json_object_set_new(response, "MessageType", json_string("GuessResponse"));
        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("No"));

        const char *player_name = json_string_value(json_object_get(message_data, "Name"));
        if( !player_name ){
            err = "Expected string field '.Data.Name' is missing or is not a string";
            goto return_error;
        }
        json_object_set_new(json_object_get(response, "Data"), "Name", json_string(player_name));

        const char *guess = json_string_value(json_object_get(message_data, "Guess"));
        if( !guess ){
            err = "Expected string field '.Data.Guess' is missing or is not a string";
            goto return_error;
        }
        json_object_set_new(json_object_get(response, "Data"), "Guess", json_string(guess));

        int rc;
        err = handle_guess(player_name, guess, sockfd, &rc);
        if( err ){
            goto return_error;
        }

        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("Yes"));

        // See handle_guess for rc values and meanings
        bool send_guess_result = rc >= 1;
        bool send_end_round = rc >= 2;
        bool send_start_round = rc == 2;
        bool send_prompt_for_guess = rc == 1 || rc == 2;
        bool send_end_game = rc == 3;
        int n_broadcasts = send_guess_result + send_end_round + send_start_round + send_prompt_for_guess + send_end_round + send_end_game;

        free(*broadcasts);
        *broadcasts = malloc((n_broadcasts + 1) * sizeof(struct broadcast_message *));
        (*broadcasts)[n_broadcasts] = NULL; // null-terminate

        // Last guess of set - send GuessResult
        if( send_guess_result ){
            json_t *guess_result_message = json_object();
            json_object_set_new(guess_result_message, "MessageType", json_string("GuessResult"));
            json_t *guess_result_message_data = json_object();
            json_t *player_info = json_array();

            bool was_winner = false;
            json_t *guess_results = assess_guesses(); // TODO: bad? do no work? or is thi not actually doing work, just transforming DB into clt-friendly
            json_object_foreach(player_to_info, player, info){
                json_t *info_to_append = json_object();
                // Don't steal references, share them
                json_object_set(info_to_append, "Name", json_object_get(info, "Name"));
                json_object_set(info_to_append, "Number", json_object_get(info, "Number"));
                json_object_set(info_to_append, "ReceiptTime", json_object_get(info, "LastGuess@"));
                // TODO: when to compute result?
                json_object_set(info_to_append, "Result", json_object_get(guess_results, player));
                // NOTE: I hate that I have to specify string "Yes" and "No": json supports the boolean type, why aren't we using it?
                json_object_set(info_to_append, "Correct", json_string(player == was_winner ? "Yes" : "No")); 
                json_array_append_new(player_info, info_to_append);
            }
            json_decref(guess_results);

            json_object_set_new(guess_result_message_data, "PlayerInfo", player_info);
            json_object_set_new(guess_result_message_data, "Winner", json_string(winner ? "Yes" : "No"));
            json_object_set_new(guess_result_message, "Data", guess_result_message_data);
            struct broadcast_message *guess_result_broadcast = broadcast_message_create(guess_result_message, NULL);
            // When present, always first
            (*broadcasts)[0] = guess_result_broadcast;
        }

        // Last guess set of round - send EndRound
        if( send_end_round ){
            json_t *end_round_message = json_object();
            json_object_set_new(end_round_message, "MessageType", json_string("EndRound"));
            json_t *end_round_message_data = json_object();

            json_t *player_info = json_array();
            json_object_foreach(player_to_info, player, info){
                json_t *info_to_append = json_object();
                // Don't steal references, share them
                json_object_set(info_to_append, "Name", json_object_get(info, "Name"));
                json_object_set(info_to_append, "Number", json_object_get(info, "Number"));
                // I wish this were total score instead of score increase...
                // Given current setup, no good way to calculate this.
                //  - start_next_round() clears RoundGuessHistory and Word from last round.
                //  - Note that I don't wish to delay invoking start_next_round() until now because it should be called by the game control hooks 'handle_X', not the dispatcher
                //  - So, I think the best way forward is to just remember the ScoreEarned computed during start_next_round() by embedding it into the player's info
                json_object_set(info_to_append, "ScoreEarned", json_object_get(info, "LastRoundScore"));
                json_object_set_new(info_to_append, "Winner", json_string(json_object_get(info, "LastGuessCorrect") == json_true() ? "Yes" : "No"));
                json_array_append_new(player_info, info_to_append);
            }
            json_object_set_new(end_round_message_data, "PlayerInfo", player_info);
            json_object_set_new(end_round_message_data, "RoundsRemaining", json_integer(N_ROUNDS - json_object_get(round_info, "RoundNumber") + 1)); // includes round just started
            json_object_set_new(end_round_message, "Data", end_round_message_data);

            struct broadcast_message *end_round_broadcast = broadcast_message_create(end_round_message, NULL);
            // When present, always second
            (*broadcasts)[1] = end_round_broadcast;
        }

        // More rounds to play - send StartRound
        if( send_start_round ){
            // When present, always third
            (*broadcasts)[2] = compile_start_round_broadcast();
        }

        // New guess set - send PromptForGuess
        if( send_prompt_for_guess ){
            // When present, always last
            (*broadcasts)[n_broadcasts - 1] = compile_prompt_for_guess_broadcast(); 
        }

        // Last round of game - send EndGame
        if( send_end_game ){
            json_t *end_game_message = json_object();
            json_object_set_new(end_game_message, "MessageType", json_string("EndGame"));
            json_t *end_game_message_data = json_object();

            json_t *player_info = json_array();
            json_object_foreach(player_to_info, player, info){
                json_t *info_to_append = json_object();
                // Don't steal references, share them
                json_object_set(info_to_append, "Name", json_object_get(info, "Name"));
                json_object_set(info_to_append, "Number", json_object_get(info, "Number"));
                json_object_set(info_to_append, "Score", json_object_get(info, "Score"));
                json_array_append_new(player_info, info_to_append);
            }
            json_object_set_new(end_game_message_data, "PlayerInfo", player_info);
            json_object_set_new(end_game_message_data, "WinnerName", json_string(get_game_winner()));
            json_object_set_new(end_game_message, "Data", end_game_message_data);

            struct broadcast_message *end_game_broadcast = broadcast_message_create(end_game_message, NULL);
            // When present, always third
            (*broadcasts)[2] = end_game_broadcast;
        }

        return response;

    }
    else {
        err = "Unrecognized MessageType. Must be one of: 'JoinInstnace', 'Guess', 'Chat'";
        json_object_set_new(response, "MessageType", json_string("InvalidRequest"));
        goto return_error;
    }
        return_error: /* LABEL */
    // TODO: Note in readme that we will include an '.Error' field in our 'Response' message detailing the error encountered if one occurs during processing
    json_object_set_new(response, "Error", json_string(err));
    return response;
    // if ladder on MessageType
    // if JoinInstance
    //   individual response = name and nonce check out ? accept : reject (JoinInstanceResponse)
    //   if we will accept player into game and then have everyone compute broadcast response (StartGame)
    //   else, broadcast result is nothing
    // if Chat, bcast message and respond nothing (Chat)
    // if Guess
    //   individual response based on whethere guess accepted (is correct length and is word?) (GuessResponse)
    //   if last player to submit guess, broadcast GuessResult: where do PromptForGuess / (EndRound + (StartRound/EndGame)) come in?
    //     - need some locking to enforce serialization so someone must be last aka avoid race condition and no one realizing they are last
    //     - idea: maybe return an array of broadcast messages, which should cause all to be broadcasted, in the order they appeared
    //       - could either have broadcast return value *always* be an array or one can check the type to determine (I think always array makes more sense)
    //
}

char *handle_join_instance(char *player_name, int nonce, int sockfd, bool *start_game_out){
    static int next_player_number = 0;

    json_t *info = json_object_get(player_to_info, player_name));
    if( !info ){
        return "No such player exists";
    }
    if( json_integer_value(json_object_get(info, "Nonce")) != nonce ){
        return "Authentication failed: incorrect nonce";
    }

    // TODO: if joined player now joining from a different socket, should we swap to expect messages from this connection instead? (seems reasonable
    if( json_object_get(info, "Disconnected@") == json_null() ){ // json_null is primitive: no mem leak and comparison works
        return "You've already joined this instance";
    }
    json_object_set_new(info, "Sockfd", json_integer(sockfd));
    // TODO: extension: implement sophisticated disconnect system
    //  - set disconnected@ in transition_to_game
    //  - in game server's main accept loop, wait minimum of 60 - (current time - player[disconnected@])
    //  - if we get timeout then, abandon game
    //  - also implement minute timeouts
    //  - problem: how to let newly rejoined player know game state (round #, word, etc)
    //      - idea: when they rejoin: send them startgame, startround, promptforguess as appropriate
    //      - this seems to require that we remember entire history for the round, and include it in promptforguess (or we can have another informative message)
    //  - need similar system for individual clt thds? or should we let chat/gibberish reqs keep the cxn alive?
    json_object_set_new(info, "Disconnected@", json_null());
    
    // If a player is reconnecting, do not reset their round guess history
    if( !json_object_get(info, "RoundGuessHistory") ){
        json_object_set_new(info, "RoundGuessHistory", json_array());
    }
    // If player is reconnecting, reuse their previously assigned number
    if( !json_object_get(info, "Number") ){
        json_object_set_new(info, "Number", json_integer(next_player_number++));
    }

    // TODO: if reconnecting, need some way to check to not trigger start_game_broadcasts to everyone?
    // - would be interesting if I could send the stat game broadcast sequence to JUST the reconnecting player
    // - might require the broadcast becoming a multicast (be able to specify recipients)
    // - otherwise, could detect a reconnect and send a special message containing all info needed to catch up (round number, word length, your guess history, all players' results history, guess number)
    *start_game_out = true;
    json_object_foreach(player_to_info, player, info){
        if( !json_equal(json_object_get(info, "Disconnected@"), json_null()) ){ // as a singleton, json_null does not need to be decref'd and thus there is no leak here
            *start_game_out = false;
            break;
        }
    }

    if( *start_game_out ){
        start_next_round(); // starts round 1
    }

    return NULL;
}

char *handle_guess(char *player_name, char *guess, int sockfd, int *rc){
    struct timespec tp; // TODO: include time.h
    clock_gettime(CLOCK_REALTIME, &tp);
    // Verify name
    char *err = verify_name(player_name, sockfd);
    if( err ) return err;

    // Verify guess
    err = verify_guess(guess);
    if( err ) return err;

    // Ensure we don't get too far ahead
    bool guess_set_finished = true;
    int n_player_guesses = json_array_size(json_object_get(json_object_get(player_to_info, player_name), "RoundGuessHistory"));
    const char *player;
    json_t *info;
    json_object_foreach(player_to_info, player, info){
        // It is possible you'll get one guess ahead of another player, but if you're already ahead you shouldn't be able to record another guess
        if( json_array_size(json_object_get(info, "RoundGuessHistory")) < n_player_guesses )
            return "You've already submitted your guess. Please wait for other players to submit their guess. You will be notified when all players have submitted and you may submit your next guess.";
        // Yours is the last guess of this guess set if everyone has more guesses than you: one equality is a witness you are not
        if( json_array_size(json_object_get(info, "RoundGuessHistory")) == n_player_guesses )
            guess_set_finished = false;
    }
    *rc = guess_set_finished;

    // Record guess
    json_array_append_new(json_object_get(json_object_get(player_to_info, player_name), "RoundGuessHistory"), json_string(guess));
    char formatted_time[BUFSIZ];
    sprintf(formatted_time, "%d.%*d", tp->tv_sec, 6, tp->nsec / 1000); // nano * 10^3 = micro
    json_object_set(json_object_get(player_to_info, player_name), "LastGuess@", json_string(formatted_time));
    
    // NOTE: could either be
    //  (a) LAZY - do minimum work now: just record answer and wait to process it later when everyone has submitted, or 
    //  (b) EAGER - do processing of this answer now, potentially eating up CPU time that could be spent helping someone else submit their answer
    // Conclusion: better to be EAGER here because we expect that players will be submitting at different times, so this gives the server something to do when it might otherwise be idle.
    char *guess_result = compute_wordle_result(guess, current_word);
    json_object_set_new(json_object_get(player_to_info, player_name), "LastGuessResult", json_string(guess_result));
    free(guess_result);

    // Check other players' statuses to set rc
    // Note: It was easy to check if all players have submitted this guess: just check if everyone has submitted the same number as me.
    //       It is a bit harder to check if game is over: have to check if any guess correct or have number of guesses reached. We also have to check whether someone won later, anyway.
    // With that said, however, we should perform these computations now because handle_guess is the game-state hook here.
    // That is, we don't want any other calls in the dispatcher altering or examining the game state, we want to encapsulate knowledge of the game state here and simply place the info needed by clt in a 'model' (certain fields of the game state). Also, we need to check if we should call start_next_round bc that call needs to be here.
    if( *rc == 1 ){
        bool last_guess_set_end = json_array_size(json_object_get(json_object_iter_value(json_object_iter(player_to_info)), "RoundGuessHistory")) == N_GUESSES;
        bool winner_end = false;
        json_object_foreach(player_to_info, player, info){
            bool guess_correct = strspn(json_string_value(json_object_get(info, "LastGuessResult"), "G") == strlen(current_word);
            if( guess_correct ){
                winner_end = true;
            }
            json_object_set_new(info, "LastGuessCorrect", json_bool(guess_correct));
        }
        bool round_end = last_guess_set_end || winner_end;
        if( round_end ) start_next_round();
        *rc += round_end;
    }
    if( *rc == 2 ){
        bool game_end = current_round == N_ROUNDS;
        *rc += game_end;
    }
    return NULL;
}

char *verify_guess(char *guess){
    if( strlen(guess) != strlen(current_word) ){
        return "Your guess is the wrong length";
    }

    return NULL;
}

// TODO: ensure consistent capitalization for keys of info of player_to_info

// Crude scoring function that gives a player 1 point if they got the word right, and 0 otherwise
int score_round(json_t *round_guess_history){
    return round_guess_history
        && !json_array_size(round_guess_history)
        && json_equals(
        json_array_get(round_guess_history, json_array_size(round_guess_history) - 1),
        json_object_get(round_info, "Word")
    );
}

// We have determined that no locking is necessary in this function.
//  - when starting game, locking not needed: locking in handle_join_instance ensures only one player can think they are last to join and initiate start game procedure.
//  - when ending round, locking not needed: locking in handle_guess ensures only one player can think they are last to guess and initiate end guess set procedure, and perhaps end round and end game procedures, too.
//  - nowhere else do we modify score, and we only read it when sending end game message to all, which will only be called from the same thread, after start_next_round is complete
//  - this is to say: this function can only be being executed
//  thought xpr: last player submission received, then ALL players submit in the background, start this fxn, cxt swt and process each player's guess, find that someone is last, and then enter here from another thread

// NOTE: after some thought, select/poll makes way more sense for this project
// Consider the following scenario in a 2-player game
//  1) Player 1 submits guess: p1 thread processes it without issue.
//  2) Player 2 submits guess: p2 locks, records guess in global, unlocks
//  3) Player 1 submits unsolicited guess (assuming the worst): suppose p1 thread then grabs the lock (BEFORE p2 can grab it to run start_next_round), sees p1 and p2 have same number of guesses, assesses p1's guess against the WRONG reference word - the next word to be chosen by start_next_round may have a different number of letters.
// This and other cases (involving when to evaluate whether a round is over) show that, in general, my critical sections are pretty much the ENTIRE processing of a response.
// So, what would make the MOST sense is to use select/poll where requests are serialized so that they do not interfere with each other in this way.
// This implementation will essentially be re-implementing select/poll in a hackish way since due to time constraints we cannot refactor into a select/poll method.
void start_next_round(){
    json_object_foreach(player_to_info, player, info){
        int current_score = json_integer_value(json_object_get(info, "Score"));
        int round_score = score_round(current_word, json_object_get(info, "RoundGuessHistory"));
        json_object_set_new(info, "LastRoundScore", json_integer(round_score));
        json_object_set_new(info, "Score", json_integer(current_score + round_score));
        json_array_clear(json_object_get(info, "RoundGuessHistory"));
    }
    char *next_word = select_word();
    json_object_set_new(round_info, "Word", json_string(next_word));
    free(next_word);
    json_object_set_new(round_info, "RoundNumber", json_integer(json_object_get(round_info, "RoundNumber") + 1)); // NOTE: if RoundNumber not yet set, json_integer will return 0, and thus we will start at round 1: perfect!
}

char *select_word(){
    size_t lineno = 0;
     // Max length: 10 chars + newline + null terminator
     // Note: We've already validated that DICT_FILE is a valid dictionary file, i.e., every word is at most 10 chars
    char selected[12];
    char current[12];
    selected[0] = '\0'; /* Don't crash if file is empty */

    fseek(DICT_FILE, 0, SEEK_SET);
    while( fgets(current, sizeof(current), DICT_FILE) ){
        if( drand48() < 1.0 / ++lineno ){
            strcpy(selected, current);
        }
    }
    size_t selectlen = strlen(selected);
    if( selectlen > 0 && selected[selectlen-1] == '\n' ){
        selected[selectlen-1] = '\0';
    }
    return strdup(selected);
}

const char *get_game_winner(){
    char *curr_arg_max = NULL;
    json_object_foreach(player_to_info, player, info){
        if( json_integer_value(json_object_get(info, "Score")) > json_integer_value(json_object_get(json_object_get(player_to_info, curr_arg_max), "Score")) ){
            curr_arg_max = player;
        }
    }
    return curr_arg_max;
}

// TODO: global round_info containing word, roundnumber

struct *broadcast_message compile_start_round_broadcast(){
    json_t *start_round_message = json_object();
    json_object_set_new(start_round_message, "MessageType", json_string("StartRound"));
    json_t *start_round_message_data = json_object(); 
    json_object_set(start_round_message_data, "Round", json_object_get(round_info, "RoundNumber"));
    json_object_set_new(start_round_message_data, "RoundsRemaining", json_integer(N_ROUNDS - json_object_get(round_info, "RoundNumber") + 1)); // equal to N_ROUNDS - current_round + 1
    json_object_set_new(start_round_message_data, "WordLength", json_integer(strlen(json_string_value(json_object_get(round_info, "Word")))));
    json_t *player_info = json_array();
    json_object_foreach(player_to_info, player, info){
        json_t *info_to_append = json_object();
        // Don't steal references, share them
        json_object_set(info_to_append, "Name", json_object_get(info, "Name"));
        json_object_set(info_to_append, "Number", json_object_get(info, "Number"));
        json_object_set(info_to_append, "Score", json_object_get(info, "Score"));
        json_array_append_new(player_info, info_to_append);
    }
    json_object_set_new(start_round_message_data, "PlayerInfo", player_info);
    json_object_set_new(start_round_message, "Data", start_round_message_data);

    struct broadcast_message *start_round_broadcast = broadcast_message_create(start_round_message, NULL);
    return start_round_broadcast;
}

struct *broadcast_message compile_prompt_for_guess_broadcast(int guess_set_num){
    json_t *prompt_for_guess_message = json_object();
    json_object_set_new(prompt_for_guess_message, "MessageType", json_string("PromptForGuess"));
    json_t *prompt_for_guess_message_data = json_object(); 
    json_object_set_new(prompt_for_guess_message_data, "WordLength", json_integer(strlen(json_string_value(json_object_get(round_info, "Word")))));
    // When we are prompting for a guess, we expect that everyone has submitted the last round of guessing, so everything has the same length GuessHistory
    json_object_set_new(prompt_for_guess_message_data, "GuessNumber", json_integer(json_array_size(json_object_get(
        json_object_iter_value(json_object_iter(player_to_info)),
        "GuessHistory"
    )));
    json_object_set_new(prompt_for_guess_message_data, "Name", json_null());
    json_object_set_new(prompt_for_guess_message, "Data", prompt_for_guess_message_data);
    struct broadcast_message *prompt_for_guess_broadcast = broadcast_message_create(prompt_for_guess_message, dynamic_field_create("Name", resolve_name), NULL);
    return prompt_for_guess_broadcast;
}


char *compute_wordle_result(char *guess, char *answer){
    // TODO: Convert guess and answer both to uppercase to compare with answer
    // TODO: should the guess I should send back also be all uppercase?
    guess = 
    // Construct multiset of letters in answer
    json_t *letters = json_object();
    for(int i = 0; i < strlen(answer): ++i ){
        char letter_as_str[2];
        letter_as_str[0] = answer[i];
        letter_as_str[0] = '\0';
        json_object_set_new(letters, letter_as_str, json_integer(json_integer_value(json_object_get(letter, letter_as_str)) + 1)); // if not present, get returns NULL, int_value returns 0, so adding 1 is fine
    }

    char *result = calloc(strlen(guess)+1, sizeof(char));
    // First consume all green letters
    for(int i = 0; i < strlen(guess); i++){
        if( guess[i] = answer[i] ){
            result[i] = "G";

            char letter_as_str[2];
            letter_as_str[0] = answer[i];
            letter_as_str[0] = '\0';
            json_object_set_new(letters, letter_as_str, json_integer(json_integer_value(json_object_get(letter, letter_as_str)) - 1)); 
        }
    }

    // Then consume all yellow letters
    // Mark only as many of each letter as are in actual word
    for(int i = 0; i < strlen(guess); i++){
        if( result[i] == "G" ) continue;

        char letter_as_str[2];
        letter_as_str[0] = answer[i];
        letter_as_str[0] = '\0';
        if( json_integer_value(json_object_get(letters, letter_as_str)) ){
            result[i] = "Y";
            json_object_set_new(letters, letter_as_str, json_integer(json_integer_value(json_object_get(letter, letter_as_str)) - 1)); 
        }
    }

    // Rest are black
    for(int i = 0; i < strlen(guess); i++){
        if( result[i] == "G" || result[i] == "Y" ) continue;
        result[i] = "B";
    }
    return result;
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
    // No reordering necessary when datatype is a single byte: endianness refers ontly to whether most- or least-significant BYTE comes first (its called BYTE order)
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

bool str_to_int(const char *str, int *int_out){
    char *end_of_int;
    int res = strtol(str, &end_of_int, 10);
    if( end_of_int != str + strlen(str) ){
        return false;
    }
    if( int_out ) *int_out = res;
    return true;
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
