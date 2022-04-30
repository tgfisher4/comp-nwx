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
#include <time.h>
#include <getopt.h>
#include <ctype.h>
#include <fcntl.h>

#include <jansson.h> // the JSON parsing library we chose to use: https://jansson.readthedocs.io/en/latest | https://github.com/akheron/jansson

#define BACKLOG 10   // how many pending connections queue will hold
#define max(a, b) ((a) > (b) ? (a) : (b))

/* Data Types */
// Note: We generally tried to use json_t for data structures as needed since we already have them, and they are very flexible and cover most use cases.
//       However, json_t has no way to store functions (understandably), so for data structures requiring function pointers, needed to use plain ol' structs.
struct broadcast_message {
    // The message to broadcast
    json_t *message;
    // For some messages, we desire that the value of certain fields is different for each player: thus the value must be resolved dynamically outside the broadcast_message specification
    // A dynamic field specifies a field within message.data to resolve dynamically, and a function that can resolve it when turning this spec into messages to send to each client
    // This is a linked list of pointers to dynamic field specs (NULL indicates end of list)
    struct dynamic_field **dynamic_data_fields;
};
struct broadcast_message *broadcast_message_create(json_t *message, ...);
void broadcast_message_destroy(struct broadcast_message *msg);

typedef json_t *(*data_resolver_t)(const char *player_name);
struct dynamic_field {
    char *name;
    data_resolver_t resolver;
};
struct dynamic_field *dynamic_field_create(char *name, data_resolver_t resolver);
void dynamic_field_destroy(struct dynamic_field *);

typedef json_t *(*message_handler_t)(json_t *message, int sockfd, struct broadcast_message ***broadcast_messages_out);

struct handle_socket_thread_wrapper_arg{
    bool is_lobby;
    int sockfd;
    message_handler_t handler;
};


// Takes the message and a NULL-terminated list of pointers to dynamic fields to embed into the broadcast
// Note: this function *steals* the jsont_t reference to message (in the jansson sense). This is assumed to be convenient for the caller in the majority of cases.
// Note: this function *steals* dynamic_fields (created via dynamic_field_create) in the sense that it will call dynamic_field_destroy on them when it is destroyed: the caller should NOT manually invoke destroy these dynamic_fields.
// That is, it allows the dynamic_fields passed to live only as long as it does (this is assumed to be a service to the caller so they don't have to maintain handles to the dynamic fields).
struct broadcast_message *broadcast_message_create(json_t *message, ...){
    struct broadcast_message *to_return = malloc(sizeof(struct broadcast_message));
    // TODO: am I fail-checking mallocs? very unlikely and not really any recourse

    // Note: NOT incref'ing the message effectively steals the reference: otherwise, we would want to incref since the caller has a reference and we are saving another.
    to_return->message = message;

    // On the first pass, just count the number of dynamic fields.
    va_list args;
    va_start(args, message);
    int n_dynamic_fields = 1; // arg list will be NULL terminated, so this count includes the NULL arg
    while( va_arg(args, struct dynamic_field *) ){ // i.e., while the next field is not NULL
        ++n_dynamic_fields;
    }
    va_end(args);

    // Even if no dynamic fields, dynamic_field will itself be a non-null double pointer pointing to a NULL single pointer.
    // In particular, this means we always need to allocate SOME space (at least one pointer's worth).
    // This creation function nicely encapsulates this subtlety.
    to_return->dynamic_data_fields = malloc(n_dynamic_fields * sizeof(struct dynamic_field *));
    va_start(args, message);
    int idx = 0;
    struct dynamic_field *field;
    while( (field = va_arg(args, struct dynamic_field *)) ){
        to_return->dynamic_data_fields[idx] = field; // steal reference
        ++idx;
    }
    to_return->dynamic_data_fields[idx] = NULL;
    va_end(args);

    return to_return;
}

void broadcast_message_destroy(struct broadcast_message *msg){
    json_decref(msg->message);
    for( int field_idx = 0; msg->dynamic_data_fields[field_idx]; ++field_idx ){
        dynamic_field_destroy(msg->dynamic_data_fields[field_idx]);
    }
    free(msg->dynamic_data_fields);
    free(msg);
}


// Note: does NOT steal the string name, but duplicates it (to allow for non-malloc'd strings)
struct dynamic_field *dynamic_field_create(char *name, data_resolver_t resolver){
    struct dynamic_field *field = malloc(sizeof(struct dynamic_field));
    field->name = strdup(name);
    field->resolver = resolver;

    return field;
}

void dynamic_field_destroy(struct dynamic_field *field){
    free(field->name);
    free(field);
}


/* Globals */
json_t *player_to_info;
json_t *round_info;
pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
int game_accept_socket;

// Default parameter values
int N_PLAYERS_PER_GAME = 2;
int LOBBY_PORT = 41000;
int GAME_PORT_START = 4101;
int N_ROUNDS = 3;
FILE *DICT_FILE = NULL;
bool DEBUG = false;
int N_GUESSES = 6; // not configurable
int TIMEOUT_SECONDS = 0; // indicates no timeout

/* Prototypes */
void usage(char *invoked_as, int exit_code);
bool process_args(int argc, char **argv);
int validate_dictionary(FILE *dict);
int bind_server_socket(char *port);
void start_server(char *port, int bound_server_socket, void (*accept_handler)(int sockfd));
void *get_in_addr(struct sockaddr *sa);
void generic_accept_handler(bool is_lobby, int sockfd, message_handler_t handler);
void *handle_socket_thread_wrapper(void *arg);
void handle_socket(bool is_lobby, int sockfd, message_handler_t handle_message);
bool dump_json(int sockfd, json_t *json);
void broadcast(struct broadcast_message *msg);
const char *get_sock_name(int sockfd);

void lobby_accept_handler(int sockfd);
json_t *lobby_message_handler(json_t *message, int sockfd, struct broadcast_message ***broadcasts);
json_t *resolve_info_field(char *field, const char *player_name);
json_t *resolve_nonce(const char *player_name);
json_t *resolve_name(const char *player_name);
char *handle_join(const char *player_name, int sockfd, int *port_out);

void transition_to_game();
char *handle_chat(const char *player_name, int sockfd, const char *text, char **text_out);
char *verify_name(const char *player_name, int sockfd);
void game_accept_handler(int sockfd);
json_t *game_message_handler(json_t *message, int sockfd, struct broadcast_message ***broadcasts);
char *handle_join_instance(const char *player_name, int nonce, int sockfd, bool *start_game_out);
char *handle_guess(const char *player_name, const char *guess, int sockfd, int *rc, bool *player_leave);
char *validate_guess(const char *guess, const char *player_name);
int score_round(json_t *round_guess_history);
void start_next_round();
char *select_word();
const char *get_game_winner();
char *compute_wordle_result(const char *guess, const char *answer);

struct broadcast_message *compile_start_round_broadcast();
struct broadcast_message *compile_prompt_for_guess_broadcast();
json_t *get_next_request_r(int sockfd, struct timeval *timeout, void **state_ptr);
bool str_to_int(const char *str, int *int_out);


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
        usage(argv[0], 1);
    }

    // Setup
    srand(time(NULL));
    signal(SIGCHLD, SIG_IGN);
    player_to_info = json_object();
    round_info = json_object();

    // Launch lobby
    char port_str[6];
    sprintf(port_str, "%d", LOBBY_PORT); // NUL-terminates
    start_server(port_str, 0, lobby_accept_handler);
}

void usage(char *invoked_as, int exit_code){
    printf(
        "Usage: %s [options]\n"
        "Options:\n"
        "   -np X: Each game includes X players. The default is 2.\n"
        "   -lp X: The main lobby server listens on port X. The default is 41000.\n"
        "   -pp X: The starting play/game port is X. When the lobby capacity is reached, a new game begins on another port, the first being X and incrementing for future games (as availability allows). The default is 4101.\n"
        "   -nr X: Each game includes X rounds. The default is 3.\n"
        "   -d dictfile: Words will be drawn from the file at path 'dictfile'. List each word on a separate line. Words must be between 3 and 10 letters, inclusive. There should NOT be a newline at the end of the file. The default dictionary is a list of past wordle answers.\n"
        "   -dbg: The servers will print debugging information whenever they receive or send a message (it does not by default).\n"
        "   -to X: The server will allow each client X seconds to guess during each round. If a player does not guess in this time, they are kicked out of the game. X=0 indicates unlimited time for each guess. The default value is 0.\n"
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
    struct option debug_on_opt = {"dbg", no_argument, NULL, 5};
    struct option timeout_seconds_opt = {"to", required_argument, NULL, 6};
    struct option sentinel = {0, 0, 0, 0};

    struct option longopts[8];
    longopts[0] = n_players_opt;
    longopts[1] = lobby_port_opt;
    longopts[2] = game_port_start_opt;
    longopts[3] = n_rounds_opt;
    longopts[4] = dict_filename_opt;
    longopts[5] = debug_on_opt;
    longopts[6] = timeout_seconds_opt;
    longopts[7] = sentinel;

    char *dict_filename = "wordle-answers-alphabetical.txt";
    int val, idx;
    while( (val = getopt_long_only(argc, argv, "", longopts, &idx)) > -1 ){
        switch(val){
            case 0:
                if( !str_to_int(optarg, &N_PLAYERS_PER_GAME) ){
                    fprintf(stderr, "option '-np' expects an integer argument.\n");
                    return false;
                }
                break;
            case 1:
                if( !str_to_int(optarg, &LOBBY_PORT) ){
                    fprintf(stderr, "option '-lp' expects an integer argument.\n");
                    return false;
                }
                if( LOBBY_PORT > 65535 ){
                    fprintf(stderr, "option '-lp' expects a valid port as argument.\n");
                    return false;
                }
                break;
            case 2:
                if( !str_to_int(optarg, &GAME_PORT_START) ){
                    fprintf(stderr, "option '-pp' expects an integer argument.\n");
                    return false;
                }
                break;
            case 3:
                if( !str_to_int(optarg, &N_ROUNDS) ){
                    fprintf(stderr, "option '-nr' expects an integer argument.\n");
                    return false;
                }
                break;
            case 4:
                dict_filename = optarg;
                break;
            case 5:
                DEBUG = true;
                break;
            case 6:
                if( !str_to_int(optarg, &TIMEOUT_SECONDS) || TIMEOUT_SECONDS < 0){
                    fprintf(stderr, "option '-to' expects an non-negative integer argument.\n");
                    return false;
                }
                break;
            default:
                printf("[INFO] Somehow reached default in process_args switch (val was %d)\n", val);
                return false;
                break; // impossible
        }
    }
    if( optind != argc ){
        fprintf(stderr, "[Error] Unrecognized option %s\n", argv[optind]);
        return false;
    }

    // Open and validate dictionary file
    DICT_FILE = fopen(dict_filename, "r");
    if( !DICT_FILE ){
        perror("couldn't open dictionary file");
        return false;
    }
    int line_err = validate_dictionary(DICT_FILE);
    if( line_err ){
        fprintf(stderr, "[Error] All words in the dictionary file must be 3-10 letters, inclusive, with one word per line and NO newline at the end of the file: line %d of %s does not comply.\n", line_err, optarg);
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
        if( !(feof(DICT_FILE) || strrchr(current, '\n')) || strlen(current) <= 3 ){
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

    if( !port ){
        // Not exactly IPv6 agnostic, but it'll do for now
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        sa.sin_port = htons(0);
        sa.sin_addr.s_addr = INADDR_ANY;

        /*
        char hostname[INET_ADDRSTRLEN];
        gethostname(hostname, INET_ADDRSTRLEN);
        inet_pton(AF_INET, hostname, &sa.sin_addr.s_addr);
        */
        bind(sockfd, (struct sockaddr *)&sa, sizeof(sa));

        return sockfd;
    }

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

    struct sockaddr_in sa;
    socklen_t sa_len = sizeof(sa);
    if (getsockname(sockfd, (struct sockaddr *)&sa, &sa_len) == -1) {
        perror("getsockname() failed");
        exit(-1); // TODO: anything more intelligent to do in this case?
    }

    char addr[INET_ADDRSTRLEN];
    inet_ntop(sa.sin_family, get_in_addr((struct sockaddr *)&(sa.sin_addr)), addr, sizeof(addr));
    /* inet_ntop(their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),
            s, sizeof s);
    */
    printf("[Info] Listening on %s:%d. Waiting for connections...\n", addr, ntohs(sa.sin_port));

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
        printf("[Info] Accepted connection from %s into socket %d\n", s, new_fd);

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
    struct handle_socket_thread_wrapper_arg *arg = malloc(sizeof(struct handle_socket_thread_wrapper_arg));
    // TODO: err check?
    arg->is_lobby = is_lobby;
    arg->sockfd = sockfd;
    arg->handler = handler;
    pthread_t thd;
    pthread_create(&thd, NULL, handle_socket_thread_wrapper, arg);
    // Since we don't intend to join the thread, we detach it immediately
    pthread_detach(thd);
}

void *handle_socket_thread_wrapper(void *arg){
    struct handle_socket_thread_wrapper_arg *typed_arg = (struct handle_socket_thread_wrapper_arg *) arg;
    handle_socket(typed_arg->is_lobby, typed_arg->sockfd, typed_arg->handler);
    free(typed_arg);
    return NULL;
}

void handle_socket(bool is_lobby, int sockfd, message_handler_t handle_message){ // data is global so it can be manipulated by all (could also accept a pointer to it)
    void *state = NULL;
    while(true){
        time_t now = time(NULL);
        const char *player_name = get_sock_name(sockfd);
        time_t deadline = json_integer_value(json_object_get(json_object_get(player_to_info, player_name), "GuessDeadline"));
        //printf("[%d] %d vs %d\n", sockfd, (int)now, (int)deadline);
        struct timeval timeout;
        timeout.tv_usec = 0;
        json_t *request;
        // If player does not currently have a deadline set, give them TIMEOUT more time
        timeout.tv_sec = deadline ? max(deadline - now, 1) : TIMEOUT_SECONDS;
        //printf("[%d] Giving %d more seconds\n", sockfd, (int)timeout.tv_sec);
        request = get_next_request_r(sockfd, &timeout, &state);
        //printf("[%d] Received a message, ready to grab lock\n", sockfd);
        
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
        //printf("[%d] Grabbed lock\n", sockfd);
        int leaving = false;
        json_t *result;
        struct broadcast_message **broadcasts;

        // Recv timed out
        if( request == json_null() ){
            //printf("[%d] Timed out %d vs %d\n", sockfd, (int)time(NULL), (int)deadline);
            // If deadline still in the future, recv again until deadline
            if( is_lobby || !deadline || deadline > time(NULL) ){
                //printf("[%d] Unlocking...\n", sockfd);
                pthread_mutex_unlock(&global_lock);
                continue;
            }
            // o/w, player has exceeded deadline: expel them
            leaving = true;
        }

        // Recv failed
        if( !request ){
            //printf("[%d] Recv failed\n", sockfd);
            if( is_lobby ){
                printf("[Info] Connection with client (via socket %d) broken: unable to recv.\n", sockfd);
                break;
            }

            // Connection has closed (ignoring the unexpected half-open case), so I would think further writes would fail.
            // However, for some reason, they don't seem to, and instead block forever.
            // After some research, it appears this is the situation: the peer closes the connection, and now no one is listening on that port; now, when data is sent to that port, under normal operations we might expect to receive a RST (peer was not expecting data) causing us to close our connection, but some DoS protection prevents us from doing so.
            // Source: https://stackoverflow.com/questions/7291940/what-may-cause-a-tcp-ip-reset-rst-flag-to-not-be-sent
            // Still, I might've expected that netcat would, when interrupted, finish the 3-way handshake with peer.
            // Unless, what's happening is that we DO complete the handshake, but then when I call send again we are attempting to set up another TCP connection? In which case it would expect a connect first, so unsolicited packets are sent RST or, as above, ignored?
            // Anyway, to get around this, we will set the send timeout to 0 for a connection suspected to have failed, so that we will detect an error immediately and not hang.
            // Yea, this didn't work either. Follow up with someone smarter to ask wtf going on.
            // Resorting to sloop boolean to hack it together
            ///*
            struct timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;
            if( setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0 ){    
                //printf("[%d] Send timeout set failed\n", sockfd);
                exit(1);
            }
            //printf("[%d] Send timeout set to 0\n", sockfd);
            leaving = 2;
        }

        if( leaving ){
            char *name = strdup(player_name);
            // Do not include leaving player in future broadcasts or bookkeeping
            json_object_del(player_to_info, get_sock_name(sockfd));
            //printf("Deleted player info bcast\n");

            // Submit dummy guess for this guess set, to trigger another check of the books so other players can move on if everyone else has submitted
            // If we have already submitted this round, this will have no effect
            request = json_object();
            json_object_set_new(request, "MessageType", json_string("Guess"));
            json_object_set_new(request, "Data", json_object());
            json_object_set_new(json_object_get(request, "Data"), "Name", json_string(name));
            // Special guess value indicating player is leaving
            json_object_set_new(json_object_get(request, "Data"), "Guess", json_string("$%"));
            free(name);
        }


        if( DEBUG ){
            fprintf(stdout, "[Debug] Received message from socket %d\n\t", sockfd);
            json_dumpf(request, stdout, 0);
            fprintf(stdout, "\n");
        }

        if( json_is_string(request) ){ //  if it's a string, it's an error message
            result = json_object();
            json_object_set_new(result, "MessageType", json_string("InvalidRequest"));
            // Set calls incref on value (respecting the caller's reference) so this should be correct.
            json_object_set_new(result, "Data", json_object());
            json_object_set(result, "Error", request);// json_copy(request));
            //json_object_set_new(result, "Error", json_copy(request));
            // No broadcasts to send in case of invalid request
            broadcasts = calloc(1, sizeof(struct broadcast_message *));
        }
        else{
            result = handle_message(request, sockfd, &broadcasts);//, database, lock); // maybe?
        }

        // If game mode, want player leaves/diconnects to be discovered by get_next_request (so I can bookkeep via dummy guess), not dump_json
        // Still want to catch disconnects in lobby here so we don't broadcast startinstance when 
        // Use hacky boolean to stop from trying to send data to closed socket, which will block forever despite me setting a timeout.
        if( result && leaving != 2 && !dump_json(sockfd, result) && is_lobby ){
            printf("[Info] Connection with client (via socket %d) broken: unable to send.\n", sockfd);
            break; // send failed, close cxn
        }
        //printf("[%d] Finished sending result\n", sockfd);

        // Once we have sent PlayerLeave to player, good to exit
        if( !is_lobby && !json_object_size(player_to_info) ){
            printf("[Info] All players left, exiting...");
            exit(1);
        }

        // Broadcast returned messages
        for( int msg_idx = 0; broadcasts[msg_idx]; ++msg_idx ){
            broadcast(broadcasts[msg_idx]);
        }
        //printf("[%d] Finished sending broadcasts\n", sockfd);

        json_decref(request);
        json_decref(result);

        // Determine if major change (transition to game or end game) needed
        int n_broadcasts = 0;
        for( ; broadcasts[n_broadcasts]; ++n_broadcasts ) ; // advance n_broadcasts until we hit NULL
        bool was_broadcast_sent = n_broadcasts > 0;

        bool was_end_game_sent = was_broadcast_sent && !strcmp(json_string_value(json_object_get(broadcasts[n_broadcasts - 1]->message, "MessageType")), "EndGame");
        bool was_start_instance_sent = was_broadcast_sent && !strcmp(json_string_value(json_object_get(broadcasts[n_broadcasts - 1]->message, "MessageType")), "StartInstance");

        // Free broadcasts
        for( int msg_idx = 0; broadcasts[msg_idx]; ++msg_idx ){
            broadcast_message_destroy(broadcasts[msg_idx]);
        }
        free(broadcasts);

        if( was_start_instance_sent ){
            // Since this thread will transform into the accept thread of the game, we won't every discover the socket we previously attended was shutdown, so just close now
            //printf("Closing socket %d\n", sockfd);
            close(sockfd);
            transition_to_game();
        }
        if( was_end_game_sent ){
            printf("Game over: exiting...\n");
            exit(1); // trust OS to free memory, close sockets, etc
        }

        // Wait to break down here so that if player leaves on last guess of game the game server will still exit
        if( leaving ) break;

        //printf("[%d] Releasing lock\n", sockfd);
        pthread_mutex_unlock(&global_lock);
        //printf("[%d] Ready to loop back\n", sockfd);
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
    //  Note: We are sure to lock in case another thread is adding a player to player_to_info (prevent del-check | add add-check start-game | del-info)
    if( is_lobby && json_object_size(player_to_info) != N_PLAYERS_PER_GAME ){
        json_object_del(player_to_info, get_sock_name(sockfd));
    }
    pthread_mutex_unlock(&global_lock);
    free(state);

    //printf("Closing socket %d\n", sockfd);
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

const char *get_sock_name(int sockfd){
    const char *player;
    json_t *info;
    json_object_foreach(player_to_info, player, info){
        if( json_integer_value(json_object_get(info, "Sockfd")) == sockfd ){
            return player;
        }
    }
    return NULL;
}

bool dump_json(int sockfd, json_t *json){
    if( DEBUG ){
        fprintf(stdout, "[Debug] Sending message to socket %d\n\t", sockfd);
        json_dumpf(json, stdout, 0);
        fprintf(stdout, "\n");
    }
    
    ///*
    struct timeval timeout;
    socklen_t len = sizeof(timeout);
    getsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, &len);
    //printf("[%d] Socket has timeout %ds and %dus\n", sockfd, (int)timeout.tv_sec, (int)timeout.tv_usec);
    //*/
    /*
    while( !strcmp(json_string_value(json_object_get(json, "MessageType")), "PlayerLeave")
            && send(sockfd, "\n", 1, 0) == 1
    ){
        printf("[%d] Sent a newline\n", sockfd);
    }
    printf("[%d] Sending newline yields %d\n", sockfd, (int)send(sockfd, "\n", 1, 0)); */
    //alarm(5);
    char *json_as_str = json_dumps(json, 0);
    //if( json_dumpfd(json, sockfd, JSON_COMPACT) < 0 || send(sockfd, "\n", 1, 0) != 1 ){ // takes care of sending over TCP (streaming) socket
    if( send(sockfd, json_as_str, strlen(json_as_str), 0) != strlen(json_as_str) || send(sockfd, "\n", 1, 0) != 1 ){ 
        fprintf(stderr, "[Error] Problem sending result back to client. Aborting connection...\n");
        return false;
    }
    free(json_as_str);
    //alarm(0);
    //printf("Send stuff with no problem\n");
    return true;
}

void broadcast(struct broadcast_message *msg){
    const char *player;
    json_t *info;
    json_object_foreach(player_to_info, player, info){
        //printf("Broadcasting to %s\n", player);
        for( int field_idx = 0; msg->dynamic_data_fields[field_idx]; ++field_idx ){
            struct dynamic_field *field = msg->dynamic_data_fields[field_idx];
            json_object_set_new(json_object_get(msg->message, "Data"), field->name, field->resolver(player));
        }
        //printf("Resolved dynamic fields for %s\n", player);
        if( !dump_json(json_integer_value(json_object_get(info, "Sockfd")), msg->message) ) ;//printf("Failed to broadcast to %s\n", player);// If one of these fails, ignore: let the 'owning' socket handle it when it discovers the socket is down
    }
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
json_t *lobby_message_handler(json_t *message, int sockfd, struct broadcast_message ***broadcasts){
    char *err = NULL;
    *broadcasts = malloc(sizeof(struct broadcast_message *));
    **broadcasts = NULL;

    json_t *response = json_object();
    json_object_set_new(response, "Data", json_object());
    json_object_set_new(response, "MessageType", json_string("InvalidRequest"));

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
        const char *player_name = json_string_value(json_object_get(message_data, "Name"));
        if( !player_name ){
            err = "Expected string field '.Data.Name' is missing or is not a string";
            goto return_error;
        }
        const char *text = json_string_value(json_object_get(message_data, "Text"));
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
        *broadcasts = malloc(2 * sizeof(struct broadcast_message *));
        (*broadcasts)[0] = chat_broadcast;
        (*broadcasts)[1] = NULL;

        json_decref(response); // chat sends no personal response to the client
        return NULL;
    }
    
    if( !strcmp(message_type, "Join") ){
        json_object_set_new(response, "MessageType", json_string("JoinResult"));
        // We expect failure by default so we'll be ready to jump to return_error without having to set this up in each case.
        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("No"));

        const char *player_name = json_string_value(json_object_get(message_data, "Name"));
        if( !player_name ){
            err = "Expected string field '.Data.Name' is missing or is not a string";
            goto return_error;
        }
        json_object_set_new(json_object_get(response, "Data"), "Name", json_string(player_name)); // go ahead and make new ref bc I'm lazy and it's just one short-lived string

        const char *client_name = json_string_value(json_object_get(message_data, "Client"));
        if( !client_name ){
            err = "Expected string field '.Data.Client' is missing or is not a string";
            goto return_error;
        }

        int port;
        err = handle_join(player_name, sockfd, &port);
        //printf("[%d] handle_join port returned: %d\n",sockfd,port);
        if( err ) goto return_error;


        // Distinguish cases with 'port': essentially a return code that also indicates new port if positive, or other cases (depending on being 0 or negative
        
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
        if( port > 0 ){ // new game server: construct broadcast message and pass through to send response message
            json_t *start_instance_data = json_object();
            
            char my_host[BUFSIZ];
            gethostname(my_host, BUFSIZ);
            // TODO: err check?
            // TODO: can we do getsockname on new socket here?
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
        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("Yes"));
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

json_t *resolve_info_field(char *field, const char *player_name){
    return json_copy(json_object_get(json_object_get(player_to_info, player_name), field));
}

json_t *resolve_nonce(const char *player_name){
    return resolve_info_field("Nonce", player_name);
}

json_t *resolve_name(const char *player_name){
    return resolve_info_field("Name", player_name);
}

char *handle_join(const char *player_name, int sockfd, int *port_out){
    *port_out = 0; // default return value (in case of error or player join but not enough for game)

    // If name already taken, return an error
    bool name_is_yours = !verify_name(player_name, sockfd);
    if( name_is_yours ){
        return "You have already joined the game lobby";
    }
    bool name_is_taken = !strcmp(player_name, "mpwordle") || json_object_get(player_to_info, player_name);
    if( name_is_taken ){
        return "That name is taken";
    }

    json_t *player_info = json_object();
    json_object_set_new(player_info, "Name", json_string(player_name));
    json_object_set_new(player_info, "Sockfd", json_integer(sockfd));
    json_object_set_new(player_info, "Nonce", json_integer(rand() % 1000000));
    json_object_set_new(player_info, "Number", json_integer(json_object_size(player_to_info)));

    json_object_set_new(player_to_info, player_name, player_info);
     
    // We decided to let the OS assign our game port, which we can then read and report to the user - this makes bookeeping easier (none) and accounts for the case that some port shortly following our start_play_port is take
    // If lobby now full, launch new game
    if( json_object_size(player_to_info) == N_PLAYERS_PER_GAME ){
        //printf("Ready to fork\n");
        if( fork() ){ // parent
            // Ignore child, go about business

            /* Close connections to lobby clients 
            //  - making sure previous clients' threads exit is important so superfluous state doesn't build up as server runs over long period
            //  - we actually don't need to do anything here because the new game server will call SHUTDOWN on all its sockets after it informs them of the new port
            //  - this shutdown call will awake threads with an error, causing them to close their sockets and exit: see transition_to_game for long-winded explanation
            const char *player_name;
            json_t *info;
            json_object_foreach(player_to_info, player_name, info){
                //close(json_integer_value(json_object_get(info, "sockfd")));
                shutdown(json_integer_value(json_object_get(info, "Sockfd")), SHUT_RDWR);
            }
            */
            
            /* Cleanup player_to_info so that we don't accumulate leak memory as we fork lobbies */
            json_object_clear(player_to_info);
            
            // Note: Interactions with player_to_info-deleting logic in handle_socket are fine
            // E.g.: Lobby locks. Chat message placed in buffers. Lobby forks, returns, and shutsdown each socket and deletes all player info. Lobby unlocks. Thread errs on shutdown socket, locks. Thread sees player_to_info not full (cleared by Lobby), searches player_to_info (nothing there), can't find, doesn't delete anything, unlocks.

            *port_out = -1; // indicate we are parent server but new game has been spawned
        } else { // child: decide game port
            game_accept_socket = bind_server_socket(NULL);
            struct sockaddr_in sa;
            socklen_t sa_len = sizeof(sa);
            if (getsockname(game_accept_socket, (struct sockaddr *)&sa, &sa_len) == -1) {
                perror("getsockname() failed");
                exit(-1); // TODO: anything more intelligent to do in this case?
            }
            *port_out = ntohs(sa.sin_port);
        }
    }
    return NULL;
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
    const char *player_name;
    json_t *info;
    json_object_foreach(player_to_info, player_name, info){
        shutdown(json_integer_value(json_object_get(info, "Sockfd")), SHUT_RDWR);
        // Mark each client as disconnected
    }
    pthread_mutex_unlock(&global_lock);
    // Start new server with slightly different accept handler
    srand48(time(NULL));
    start_server(NULL, game_accept_socket, game_accept_handler);
}

char *handle_chat(const char *player_name, int sockfd, const char *text, char **text_out){
    char *err = verify_name(player_name, sockfd);
    if( err ) return err;

    *text_out = strdup(text);
    return NULL;
}

char *verify_name(const char *player_name, int sockfd){
    if( sockfd != json_integer_value(json_object_get(json_object_get(player_to_info, player_name), "Sockfd")) )
        return "That's not your name, or you have not yet joined.";
    return NULL;
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
// One may note the similar structure of game_message_handler and lobby_message_handler and wonder: is a generalization possible/desirable, taking a specification of functions to invoke for each message_type?
//  - might be cool, but at some point need to get this project done
//  - might be desirable if more, similarly structured handlers were expected, but they are not
json_t *game_message_handler(json_t *message, int sockfd, struct broadcast_message ***broadcasts){
    char *err = NULL;
    // Default to no broadcasts
    *broadcasts = malloc(sizeof(struct broadcast_message *));
    **broadcasts = NULL;

    json_t *response = json_object();
    json_object_set_new(response, "Data", json_object());
    json_object_set_new(response, "MessageType", json_string("InvalidRequest"));

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

    // Looping variables everyone in this function can use
    const char *player;
    json_t *info;//, *tmp;

    if( !strcmp(message_type, "Chat") ){
        json_object_set_new(response, "MessageType", json_string("ChatResult"));

        // Should we send an error message to the client if they don't send a valid chat?
        const char *player_name = json_string_value(json_object_get(message_data, "Name"));
        if( !player_name ){
            err = "Expected string field '.Data.Name' is missing or is not a string";
            goto return_error;
        }
        const char *text = json_string_value(json_object_get(message_data, "Text"));
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
        *broadcasts = malloc(2 * sizeof(struct broadcast_message *));
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
        // Probably optimal to add another reference to the player_name string in the message_data object, but this is only one string and is short-lived: both message and response will be freed at the same time
        json_object_set_new(json_object_get(response, "Data"), "Name", json_string(player_name));

        int nonce = json_integer_value(json_object_get(message_data, "Nonce"));
        if( !nonce ){
            err = "Expected integer field '.Data.Nonce' is missing or is not an integer";
            goto return_error;
        }

        bool start_game;// = false;
        err = handle_join_instance(player_name, nonce, sockfd, &start_game);
        //printf("Handled joined instance\n");
        if( err ){
            goto return_error;
        }

        json_object_set_new(json_object_get(response, "Data"), "Result", json_string("Yes"));
        json_object_set(json_object_get(response, "Data"), "Number", json_object_get(json_object_get(player_to_info, player_name), "Number"));

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
        
        //printf("Constructed star game broadcasts\n");
        return response;
    }
    else if( !strcmp(message_type, "Guess") ){
        json_object_set_new(response, "MessageType", json_string("GuessResponse"));
        json_object_set_new(json_object_get(response, "Data"), "Accepted", json_string("No"));

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
        char *uppercase_guess = strdup(guess);
        for(int i = 0; i < strlen(guess); i++) uppercase_guess[i] = toupper(guess[i]);
        json_object_set_new(json_object_get(response, "Data"), "Guess", json_string(uppercase_guess));

        int rc;
        bool player_leave;
        err = handle_guess(player_name, uppercase_guess, sockfd, &rc, &player_leave);
        free(uppercase_guess);
        //printf("Handle guess rc: %d\n", rc);
        if( err ){
            goto return_error;
        }

        json_object_set_new(json_object_get(response, "Data"), "Accepted", json_string("Yes"));

        // See handle_guess for rc values and meanings
        bool send_player_leave = player_leave;
        bool send_guess_result = rc >= 1;
        bool send_end_round = rc >= 2;
        bool send_start_round = rc == 2;
        bool send_prompt_for_guess = (rc == 1 || rc == 2);
        bool send_end_game = rc == 3;
        int n_broadcasts = send_player_leave + send_guess_result + send_end_round + send_start_round + send_prompt_for_guess + send_end_game;

        free(*broadcasts); // free placeholder
        *broadcasts = malloc((n_broadcasts + 1) * sizeof(struct broadcast_message *));
        (*broadcasts)[n_broadcasts] = NULL; // null-terminate

        if( send_player_leave ){
            json_t *player_leave_message = json_object();
            json_object_set_new(player_leave_message, "MessageType", json_string("PlayerLeave"));
            json_object_set_new(player_leave_message, "Data", json_object());
            json_object_set_new(json_object_get(player_leave_message, "Data"), "Name", json_string(player_name));
            struct broadcast_message *player_leave_broadcast = broadcast_message_create(player_leave_message, NULL);
            // When present, always first
            (*broadcasts)[0] = player_leave_broadcast;
        }

        // Last guess of set - send GuessResult
        if( send_guess_result ){
            json_t *guess_result_message = json_object();
            json_object_set_new(guess_result_message, "MessageType", json_string("GuessResult"));
            json_t *guess_result_message_data = json_object();
            json_t *player_info = json_array();

            bool was_winner = false;
            json_object_foreach(player_to_info, player, info){
            /*

                // If player is leaving, don't include their result
                if( !strcmp("$%", json_string_value(json_array_get(
                        json_object_get(info, "RoundGuessHistory"),
                        json_array_size(json_object_get(info, "RoundGuessHistory") - 1
                )))) ){
                    continue;
                }
                */

                json_t *last_guess_correct = json_object_get(info, "LastGuessCorrect");
                json_t *info_to_append = json_object();
                // Don't steal references, share them
                json_object_set(info_to_append, "Name", json_object_get(info, "Name"));
                json_object_set(info_to_append, "Number", json_object_get(info, "Number"));
                json_object_set(info_to_append, "ReceiptTime", json_object_get(info, "LastGuess@"));
                json_object_set(info_to_append, "Result", json_object_get(info, "LastGuessResult"));
                // NOTE: Why are we using a string to specify "Yes" and "No": json supports the boolean type, why aren't we using it?
                was_winner = was_winner || last_guess_correct == json_true();
                json_object_set_new(info_to_append, "Correct", json_string(last_guess_correct == json_true() ? "Yes" : "No"));
                json_array_append_new(player_info, info_to_append);
            }

            json_object_set_new(guess_result_message_data, "PlayerInfo", player_info);
            json_object_set_new(guess_result_message_data, "Winner", json_string(was_winner ? "Yes" : "No"));
            json_object_set_new(guess_result_message, "Data", guess_result_message_data);
            struct broadcast_message *guess_result_broadcast = broadcast_message_create(guess_result_message, NULL);
            // When present, comes after player_leave (if present)
            (*broadcasts)[send_player_leave] = guess_result_broadcast;
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
            json_object_set_new(end_round_message_data, "RoundsRemaining", json_integer(N_ROUNDS - json_integer_value(json_object_get(round_info, "RoundNumber")) + 1)); // includes round just started
            json_object_set_new(end_round_message, "Data", end_round_message_data);

            struct broadcast_message *end_round_broadcast = broadcast_message_create(end_round_message, NULL);
            // When present, comes after GuessResult (if present)
            (*broadcasts)[1 + send_player_leave] = end_round_broadcast;
        }

        // More rounds to play - send StartRound
        if( send_start_round ){
            // When present, comes after EndRound
            (*broadcasts)[2 + send_player_leave] = compile_start_round_broadcast();
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
            // When present, comes last
            (*broadcasts)[n_broadcasts - 1] = end_game_broadcast;
        }

        // Return a leave message instead of the normal response
        if( send_player_leave ){
            json_decref(response);
            json_t *player_leave_message = (*broadcasts)[0]->message;
            json_incref(player_leave_message);
            return player_leave_message;
        }

        return response;

    }
    else {
        err = "Unrecognized MessageType. Must be one of: 'JoinInstance', 'Guess', 'Chat'";
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

char *handle_join_instance(const char *player_name, int nonce, int sockfd, bool *start_game_out){
    json_t *player_info = json_object_get(player_to_info, player_name);
    if( !player_info ){
        return "No such player exists";
    }
    if( json_integer_value(json_object_get(player_info, "Nonce")) != nonce ){
        return "Authentication failed: incorrect nonce";
    }

    // TODO: if joined player now joining from a different socket, should we swap to expect messages from this connection instead? (seems reasonable)
    if( json_object_get(player_info, "Disconnected@") == json_null() ){ // json_null is primitive: no mem leak and comparison works
        return "You've already joined this instance";
    }
    json_object_set_new(player_info, "Sockfd", json_integer(sockfd));
    // TODO: extension: implement sophisticated disconnect system
    //  - set disconnected@ in transition_to_game
    //  - in game server's main accept loop, wait minimum of 60 - (current time - player[disconnected@])
    //  - if we get timeout then, abandon game
    //  - also implement minute timeouts
    //  - problem: how to let newly rejoined player know game state (round #, word, etc)
    //      - idea: when they rejoin: send them startgame, startround, promptforguess as appropriate
    //      - this seems to require that we remember entire history for the round, and include it in promptforguess (or we can have another informative message)
    //  - need similar system for individual clt thds? or should we let chat/gibberish reqs keep the cxn alive?
    json_object_set_new(player_info, "Disconnected@", json_null());
    
    // If a player is reconnecting, do not reset their round guess history
    if( !json_object_get(player_info, "RoundGuessHistory") ){
        json_object_set_new(player_info, "RoundGuessHistory", json_array());
    }
    // If player is reconnecting, reuse their previously assigned number
    /*
    if( !json_object_get(player_info, "Number") ){
    }
    */

    // TODO: if reconnecting, need some way to check to not trigger start_game_broadcasts to everyone?
    // - would be interesting if I could send the stat game broadcast sequence to JUST the reconnecting player
    // - might require the broadcast becoming a multicast (be able to specify recipients)
    // - otherwise, could detect a reconnect and send a special message containing all info needed to catch up (round number, word length, your guess history, all players' results history, guess number)
    *start_game_out = true;
    const char *player;
    json_t *info;
    json_object_foreach(player_to_info, player, info){
        if( !json_equal(json_object_get(info, "Disconnected@"), json_null()) ){ // as a singleton, json_null does not need to be decref'd and thus there is no leak here
            *start_game_out = false;
            break;
        }
    }

    if( *start_game_out ){
        start_next_round(); // starts round 1
        json_t *deadline = json_integer(
                TIMEOUT_SECONDS ? time(NULL) + TIMEOUT_SECONDS : 0
        );
        const char *player;
        json_t *info;
        // Start guess 1 timer
        json_object_foreach(player_to_info, player, info){
            json_object_set(info, "GuessDeadline", deadline);
        }
        //printf("Set deadline to %d\n", (int)json_integer_value(deadline));
        json_decref(deadline);
    }

    return NULL;
}

char *handle_guess(const char *player_name, const char *guess, int sockfd, int *rc, bool *player_leave){
    *player_leave = !strcmp(guess, "$%");
    // If player left, don't bookkeep their nonexistant records, just check again if we've reached end of round now that this player's records are gone
    if( !*player_leave ){
        struct timespec tp;
        clock_gettime(CLOCK_REALTIME, &tp);
        // Verify name
        char *err = verify_name(player_name, sockfd);
        if( err ) return err;

        // Validate guess
        err = validate_guess(guess, player_name);
        if( err ) return err;

        // Record guess
        json_t *info = json_object_get(player_to_info, player_name);
        json_array_append_new(json_object_get(info, "RoundGuessHistory"), json_string(guess));
        char formatted_time[BUFSIZ];
        sprintf(formatted_time, "%lld.%06lld", (long long) tp.tv_sec, (long long) tp.tv_nsec / 1000); // nano * 10^3 = micro
        json_object_set_new(info, "LastGuess@", json_string(formatted_time));
        json_object_set_new(info, "GuessDeadline", json_integer(0));
    
    // NOTE: could either be
    //  (a) LAZY - do minimum work now: just record answer and wait to process it later when everyone has submitted, or 
    //  (b) EAGER - do processing of this answer now, potentially eating up CPU time that could be spent helping someone else submit their answer
    // Conclusion: better to be EAGER here because we expect that players will be submitting at different times, so this gives the server something to do when it might otherwise be idle.
        char *guess_result = compute_wordle_result(guess, json_string_value(json_object_get(round_info, "Word")));
        bool guess_correct = strspn(guess_result, "G") == strlen(guess_result);
        json_object_set_new(info, "LastGuessResult", json_string(guess_result));
        json_object_set_new(info, "LastGuessCorrect", json_boolean(guess_correct));
        free(guess_result);
    }

    // Check other players' statuses to set rc
    // Note: It was easy to check if all players have submitted this guess: just check if everyone has submitted the same number as me.
    //       It is a bit harder to check if game is over: have to check if any guess correct or have number of guesses reached. We also have to check whether someone won later, anyway.
    // With that said, however, we should perform these computations now because handle_guess is the game-state hook here.
    // That is, we don't want any other calls in the dispatcher altering or examining the game state, we want to encapsulate knowledge of the game state here and simply place the info needed by clt in a 'model' (certain fields of the game state). Also, we need to check if we should call start_next_round bc that call needs to be here.
    *rc = 1;
    const char *player;
    json_t *info;
    // A guess set is complete if all players have submitted a guess, which we hackily determine through the LastGuess@ field: we reset this when sending GuessResult.
    json_object_foreach(player_to_info, player, info){
        if( !json_object_get(info, "LastGuess@") )
            *rc = 0;
    }
    if( *rc == 1 ){
        int n_guesses = json_array_size(json_object_get(
                json_object_iter_value(json_object_iter(player_to_info)),
                "RoundGuessHistory"
        ));
        bool last_guess_set_end = n_guesses == N_GUESSES;
        bool winner_end = false;
        json_object_foreach(player_to_info, player, info){
            winner_end = winner_end || json_object_get(info, "LastGuessCorrect") == json_true();
        }
        bool round_end = last_guess_set_end || winner_end;
        if( round_end ) start_next_round();
        *rc += round_end;
    }
    if( *rc == 2 ){
        bool game_end = json_integer_value(json_object_get(round_info, "RoundNumber")) > N_ROUNDS;
        *rc += game_end;
    }
    if( *rc >= 1 ){
        json_t *deadline = json_integer(
                TIMEOUT_SECONDS ? time(NULL) + TIMEOUT_SECONDS : 0
        );
        const char *player;
        json_t *info;
        json_object_foreach(player_to_info, player, info){
            json_object_set(info, "GuessDeadline", deadline);
            json_object_del(info, "LastGuess@");
        }
        //printf("Set deadline to %d\n", (int)json_integer_value(deadline));
        json_decref(deadline);
    }
    return NULL;
}

char *validate_guess(const char *guess, const char *player_name){
    if( strlen(guess) != json_string_length(json_object_get(round_info, "Word")) ){
        return "Your guess is the wrong length";
    }

    // If player has already submitted this round, don't let them submit again. 
    // Determine this through the LastGuess@ field.
    if( json_object_get(json_object_get(player_to_info, player_name), "LastGuess@") ){
        return "You've already submitted your guess. Please wait for other players to submit their guess. You will be notified when all players have submitted and you may submit your next guess.";
    }

    return NULL;
}

// Crude scoring function that gives a player 1 point if they got the word right, and 0 otherwise
int score_round(json_t *round_guess_history){
    return json_array_size(round_guess_history)
        && json_equal(
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
    const char *player;
    json_t *info;
    json_object_foreach(player_to_info, player, info){
        int current_score = json_integer_value(json_object_get(info, "Score"));
        int round_score = score_round(json_object_get(info, "RoundGuessHistory"));
        json_object_set_new(info, "LastRoundScore", json_integer(round_score));
        json_object_set_new(info, "Score", json_integer(current_score + round_score));
        json_array_clear(json_object_get(info, "RoundGuessHistory"));
    }
    char *next_word = select_word();
    json_object_set_new(round_info, "Word", json_string(next_word));
    free(next_word);
    // NOTE: if RoundNumber not yet set, json_integer will return 0, and thus we will start at round 1: perfect!
    json_object_set_new(round_info, "RoundNumber", json_integer(json_integer_value(json_object_get(round_info, "RoundNumber")) + 1));
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
    char *uppercase_selected = strdup(selected);
    for(int i = 0; i < strlen(selected); i++) uppercase_selected[i] = toupper(selected[i]);
    //printf("WORD FOR ROUND: %s\n", uppercase_selected);
    return uppercase_selected;
}

const char *get_game_winner(){
    const char *curr_arg_max = json_string_value(json_object_get(json_object_iter_value(json_object_iter(player_to_info)), "Name"));
    const char *player;
    json_t *info;
    json_object_foreach(player_to_info, player, info){
        if( json_integer_value(json_object_get(info, "Score")) > json_integer_value(json_object_get(json_object_get(player_to_info, curr_arg_max), "Score")) ){
            curr_arg_max = player;
        }
    }
    return curr_arg_max;
}

struct broadcast_message *compile_start_round_broadcast(){
    json_t *start_round_message = json_object();
    json_object_set_new(start_round_message, "MessageType", json_string("StartRound"));
    json_t *start_round_message_data = json_object(); 
    json_object_set(start_round_message_data, "Round", json_object_get(round_info, "RoundNumber"));
    json_object_set_new(start_round_message_data, "RoundsRemaining", json_integer(N_ROUNDS - json_integer_value(json_object_get(round_info, "RoundNumber")) + 1)); // equal to N_ROUNDS - current_round + 1
    json_object_set_new(start_round_message_data, "WordLength", json_integer(json_string_length(json_object_get(round_info, "Word"))));
    json_t *player_info = json_array();
    const char *player;
    json_t *info;
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

struct broadcast_message *compile_prompt_for_guess_broadcast(){
    json_t *prompt_for_guess_message = json_object();
    json_object_set_new(prompt_for_guess_message, "MessageType", json_string("PromptForGuess"));
    json_t *prompt_for_guess_message_data = json_object(); 
    json_object_set_new(prompt_for_guess_message_data, "WordLength", json_integer(json_string_length(json_object_get(round_info, "Word"))));
    json_object_set(prompt_for_guess_message_data, "GuessDeadline", json_object_get(round_info, "GuessDeadline"));
    // When we are prompting for a guess, we expect that everyone has submitted the last round of guessing, so everything has the same length GuessHistory
    json_object_set_new(prompt_for_guess_message_data, "GuessNumber", json_integer(1 + json_array_size(json_object_get(
        json_object_iter_value(json_object_iter(player_to_info)),
        "RoundGuessHistory"
    ))));
    json_object_set_new(prompt_for_guess_message_data, "Name", json_null());
    json_object_set_new(prompt_for_guess_message, "Data", prompt_for_guess_message_data);
    struct broadcast_message *prompt_for_guess_broadcast = broadcast_message_create(prompt_for_guess_message, dynamic_field_create("Name", resolve_name), NULL);
    return prompt_for_guess_broadcast;
}


char *compute_wordle_result(const char *guess, const char *answer){
    // Construct multiset of letters in answer
    json_t *letters = json_object();
    for(int i = 0; i < strlen(answer); ++i ){
        char letter_as_str[2];
        letter_as_str[0] = toupper(answer[i]);
        letter_as_str[1] = '\0';
        json_object_set_new(letters, letter_as_str, json_integer(json_integer_value(json_object_get(letters, letter_as_str)) + 1)); // if not present, get returns NULL, int_value returns 0, so adding 1 is fine
    }

    // Use calloc so we don't receive stale G's or Y's
    char *result = calloc(strlen(guess)+1, sizeof(char));
    result[strlen(guess)] = '\0';

    // First consume all green letters
    for(int i = 0; i < strlen(guess); i++){
        if( toupper(guess[i]) == toupper(answer[i]) ){
            result[i] = 'G';

            char letter_as_str[2];
            letter_as_str[0] = toupper(answer[i]);
            letter_as_str[1] = '\0';
            json_object_set_new(letters, letter_as_str, json_integer(json_integer_value(json_object_get(letters, letter_as_str)) - 1)); 
        }
    }

    // Then consume all yellow letters
    // Mark only as many of each letter as are in actual word
    for(int i = 0; i < strlen(guess); i++){
        if( result[i] == 'G' ) continue;

        char letter_as_str[2];
        letter_as_str[0] = toupper(guess[i]);
        letter_as_str[1] = '\0';
        if( json_integer_value(json_object_get(letters, letter_as_str)) ){
            result[i] = 'Y';
            json_object_set_new(letters, letter_as_str, json_integer(json_integer_value(json_object_get(letters, letter_as_str)) - 1)); 
        }
    }

    // Rest are black
    for(int i = 0; i < strlen(guess); i++){
        if( result[i] == 'G' || result[i] == 'Y' ) continue;
        result[i] = 'B';
    }
    return result;
}

// Double pointer to state so we can read/write it.
// *state MUST be initialized to NULL on first invocation.
json_t *get_next_request_r(int sockfd, struct timeval *timeout, void **state_ptr){
    if( !*state_ptr ){
        *state_ptr = malloc(sizeof(char *) + sizeof(int));
        if( !*state_ptr ){
            fprintf(stderr, "[Error] Memory allocation failed: %s. Aborting...\n", strerror(errno));
            exit(1);
        }

        *(char **)*state_ptr = malloc(BUFSIZ);
        if( !*(char **)*state_ptr  ){
            fprintf(stderr, "[Error] Memory allocation failed: %s. Aborting...\n", strerror(errno));
            exit(1);
        }
        *(int *)((char **)*state_ptr + 1) = BUFSIZ;
    }

    char *buffer;
    int space_remaining; // essentially tracks end of meaningful data in buffer
    // Recover state from opaque pointer.
    // State format: state --> |char * buffer|int space_remaining|
    buffer = *(char **)*state_ptr; // Pretend its a char **
    space_remaining = *(int *)((char **)*state_ptr + 1); // Pretend its char **, go to next char *, pretend its int * instead, and deref

    // No reordering necessary when datatype is a single byte: endianness refers ontly to whether most- or least-significant BYTE comes first (its called BYTE order)
    int current_bufsiz_multiple = 1; // resets to 1 on every invocation
    do {
        int space_used = current_bufsiz_multiple * BUFSIZ - space_remaining;
        char *unused_buffer_start = buffer + space_used;

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
                exit(1);
            }
            strncpy(small_buffer, newline + 1, n_bytes_to_preserve);
            free(buffer); // release space after each request: if we keep adding onto same buffer we will consume lots of memory we aren't using
            buffer = small_buffer;
            space_remaining = BUFSIZ - n_bytes_to_preserve; // everything beyond what we will preserve is free buffer space

            // Pack state into opaque pointer
            *(char **)*state_ptr = buffer;
            *(int *)((char **)*state_ptr + 1) = space_remaining;

            return to_return;
        }

        // If buffer full, allocate more space: keep it contiguous so we can JSON parse it all together
        if( space_remaining == 0 ){
            current_bufsiz_multiple += 1;
            buffer = realloc(buffer, current_bufsiz_multiple * BUFSIZ);
            if( !buffer ){
                fprintf(stderr, "[Error] Memory allocation failed: %s. Aborting...\n", strerror(errno));
                exit(1);
            }
            space_remaining = BUFSIZ;
        }

        // Receive more data to try to find end of request
        //printf("[%d] ABOUT TO RECV: OFFSET %ld, UP TO %d\n", sockfd, unused_buffer_start - buffer, space_remaining);
        // If timeout NULL, set timeout to 0, which causes recv to never timeout
        struct timeval tv;
        if( timeout ) tv = *timeout;
        else {
            tv.tv_sec = 0;
            tv.tv_usec = 0;
        }
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
         
        int got = recv(sockfd, unused_buffer_start, space_remaining, 0);
        //printf("[%d] RETURNED FROM RECV: GOT %d\n", sockfd, got);
        if( got < 0 ){
            if( errno == EAGAIN || errno == EWOULDBLOCK ) return json_null();
            perror("[Error] Could not receive from client");
            return NULL;
        }
        if( got == 0 ){
            return NULL;
        }
        space_remaining -= got;
        
        //printf("[%d] BUFFER (length %d, %d remaining) \n\t|%.*s|\n", sockfd, current_bufsiz_multiple * BUFSIZ - space_remaining, space_remaining, current_bufsiz_multiple * BUFSIZ - space_remaining, buffer);

        //printf("[%d] BUFFER \n\t|", sockfd);
        //for( int i = 0; i < current_bufsiz_multiple * BUFSIZ - space_remaining; ++i)
            //printf("(%c, %d)", buffer[i], buffer[i]);
        //printf("|\n");
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

