''' Wordle client '''

import sys
import time
import json
import socket
import threading
import string

import utils

import curses
from curses.textpad import Textbox, rectangle

''' Idea: different users chats are in a certain color '''

skt = None

class WordleClient:
    def __init__(self, username, srvr_hostname, lobby_port, stdscr):
        self.username = username
        self.mode = 'chat' # other option is 'guess'; current user mode
        # Open socket and attempt to join server
        self.join((srvr_hostname, lobby_port), 'Join', 'BrownFisherGallagher-Python')

        ''' Curses UI ''' 
        self.stdscr = stdscr
        # Clear terminal
        self.stdscr.clear()
        
        ''' Curses preliminaries '''
        # Store entire screen size
        self.H = curses.LINES-5; self.W = curses.COLS-5
        
        # Check for and begin color support
        if curses.has_colors():
            curses.start_color()
        # Initialize color combinations
        curses.init_pair(1, curses.COLOR_RED, curses.COLOR_BLACK)
        curses.init_pair(2, curses.COLOR_GREEN, curses.COLOR_BLACK)
        curses.init_pair(3, curses.COLOR_BLUE, curses.COLOR_BLACK)
        curses.init_pair(4, curses.COLOR_YELLOW, curses.COLOR_BLACK)
        curses.init_pair(5, curses.COLOR_CYAN, curses.COLOR_BLACK)
        curses.init_pair(6, curses.COLOR_MAGENTA, curses.COLOR_BLACK)
        curses.init_pair(7, curses.COLOR_WHITE, curses.COLOR_BLACK)

        ''' 3 horiz window panes Top,Middle,Bottom: (1) Game state, (2) Chat/Info, (3) User prompt '''
        self.state_win = curses.newwin(int(self.H/6), self.W, 0, 0)
        self.chat_pad = curses.newpad(self.H, self.W)
        # prompt window 
        w = self.W-10 # would prefer terminal width
        tl_y = self.H+3; tl_x = 0+20 # pad by 20
        self.editwin = curses.newwin(1, int(w/4), tl_y, tl_x+1)
        
        # Display current mode and how to toggle
        self.disp_toggle_instr()
        
        # Display board
        self.last_guess = 'WORDLE'
        self.result = 'G'*6
        self.guess_num = 1
        self.guesses = 6 # Rule of the real game
        self.d = {'B': 7, 'G': 2, 'Y' : 4} # color map for guesses
        self.show_board()
        
        # Map players to chat colors as we see them 
        self.p_to_color = {}

        # 3 Thread Approach (Receiver/Joiner, Sender, UI Refresher)
        # Block for user prompt responses/chat messages
        threading.Thread(target=self.await_input, daemon=True).start() 

        # Print ongoing chat and game feed
        threading.Thread(target=self.render_msgs, daemon=True).start()

        # Dedicated thread to Refresh UI
        while True:
            time.sleep(0.1)
            stdscr.noutrefresh()
            self.editwin.noutrefresh()
            curses.doupdate()

    # Connects global socket to lobby/game instance
    def join(self, addr, msg_type, unique_id): 
        global skt
        temp_skt = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        temp_skt.connect(addr)

        data = {}
        data['Name'] = self.username
        if isinstance(unique_id, str):
            data['Client'] = unique_id
        else:
            data['Nonce'] = unique_id
        msg = {
                'MessageType': msg_type, "Data": data
                }
        utils.send_nl_message(temp_skt, utils.encode_object(msg)) # do i need to encode?
        skt = temp_skt
        self.skt_msgs = utils.nl_socket_messages(skt)

    # Source: https://www.programcreek.com/python/example/106575/curses.textpad
    # Introduces backspace, enter capabilities
    def _search_validator(self, ch):
        """Fix Enter and backspace for textbox.
        Used as an aux function for the textpad.edit method
        """
        if ch == curses.ascii.NL:  # Enter
            return curses.ascii.BEL
        elif ch == 127:  # Backspace
            self.search_str = self.box.gather().strip().lower()[:-1]
            return 8
        else: # Toggle modes on $, >
            if 0 < ch < 256:
                c = chr(ch) 
                if c == '$':
                    if self.mode != 'guess':
                        self.mode = 'guess' 
                        self.disp_toggle_instr()
                    return 0
                elif c == '>':
                    if self.mode != 'chat':
                        self.mode = 'chat' 
                        self.disp_toggle_instr()
                    return 0
            return ch

    # Current message type and how to switch UI display
    def toggle_instr(self, act, other):
        self.stdscr.addstr(curses.LINES-1, 0, act, curses.A_BOLD | curses.color_pair(3))
        self.stdscr.addstr(curses.LINES-1, 20+int(self.W/2), other)
        
    def disp_toggle_instr(self):
        if self.mode == 'guess':
            self.toggle_instr("GUESSING.", "Press '>' to chat ") # xtra space deletes 's'
        else:
            self.toggle_instr("CHATTING.", "Press '$' to guess")
        
        self.stdscr.noutrefresh()
        self.editwin.noutrefresh()

    # Display user's pvs guess results UI
    def clear_board(self):
        for i in range(12): # Max num of guesses?
            self.stdscr.addstr(i, 0, ' '*30) # Max word len *3?
        self.stdscr.noutrefresh()

    def show_board(self):
        s = '  '.join(list(self.last_guess))
        s += ' '*5
        self.stdscr.addstr(self.guess_num, 0, s)
        
        for idx, color in enumerate(self.result):
            self.stdscr.chgat(self.guess_num, idx*3, curses.A_BOLD | curses.color_pair(self.d[color]))
        self.stdscr.noutrefresh()

    # Increment cursor while keeping in bounds
    def cursor_increment(self):
        self.curr_chatln += 1
        # Run out of room in pad; delete first line
        if self.curr_chatln > self.H-5:
            self.chat_pad.move(0,0)
            self.chat_pad.deleteln()
            self.curr_chatln -= 1
   

    # Game events in chat
    def p_begin_round(self, infos):
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'Beginning Round {self.round} of {self.rounds}.')
        for i, p in enumerate(sorted(infos, key=lambda p:p['Score'])):
            self.chat_pad.addstr(self.curr_chatln, 0, f'\t{i+1}) {p["Name"]}: {p["Score"]} pts')
            self.cursor_increment()
    
    def p_post_guess(self, infos):
        # Note that we don't use number, receiptTime, or correct (see winner next) 
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'Guess Results - ')
        self.cursor_increment()
        for i, p in enumerate(sorted(infos, key=lambda p:p['Number'])):
            result = p['Result'] 
            if p['Name'] == self.username:
                self.result = result
                #continue
            
            s = ' '.join(list('@'*self.wordLength))
            t = '    '
            s0 = t+ f'{p["Name"]}:'+t
            s = s0 + s
            self.chat_pad.addstr(self.curr_chatln-1, 0, s)

            loc = len(s0)
            for idx, color in enumerate(self.result):
                self.chat_pad.chgat(self.curr_chatln-1, loc-1+idx*2, curses.A_BOLD | curses.color_pair(self.d[color]))
            self.cursor_increment()
    
    def p_post_round(self, infos):
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'Round {self.round} of {self.rounds} complete.')
        winners = [ p for p in infos if p['Winner']=='Yes' ]
        s = 'Winners: '
        for w in winners:
            s += f'Winners: {w["Name"]} (+{w["ScoreEarned"]} '
        self.chat_pad.addstr(self.curr_chatln, 0, s)
        self.cursor_increment()
    
    def p_post_game(self, infos):
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'That concludes our game.')
        self.cursor_increment()
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'Congratulations to our winner: {self.winner}!')
        self.cursor_increment()
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'Final results - ')
        for i, p in enumerate(sorted(infos, key=lambda p:p['Score'])):
            self.chat_pad.addstr(self.curr_chatln, 0, f'\t{i+1}) {p["Name"]}: {p["Score"]} pts')
            self.cursor_increment()
    
    # Receiver thread to act based on msg supplied by server
    def process_msg(self, msg):

        if msg['MessageType'] == 'JoinResult':
            # Receiver in charge of joining so already knows name...nothing to see here.
            # Log to chat and if lobby server couldn't be joined, probably didn't choose unique name. Exiting...
            if msg['Data']['Result'] == 'Yes':
                self.chat_pad.addstr(self.curr_chatln-1, 0, f'{self.username} has joined the lobby.')
                self.cursor_increment()
            else:
                sys.exit(1)

        elif msg['MessageType'] == 'InvalidMessage':
            self.chat_pad.addstr(self.curr_chatln-1, 0, f"Oops guessing is not allowed yet. You're still in the lobby!")
            self.cursor_increment()

        # Note that server will never send a msg with "Chat" type under name 'mpwordle'
        elif msg['MessageType'] == 'Chat':
            # New teammate discovered! Remember his color and post his msg.
            player = msg["Data"]["Name"]
            if not player in self.p_to_color:
                self.p_to_color[player] = len(self.p_to_color)%6+1
            self.chat_pad.addstr(self.curr_chatln-1, 0, f'{player}: {msg["Data"]["Text"]}', curses.A_ITALIC | curses.color_pair(self.p_to_color[player]))
            self.cursor_increment()
            
        # Note that receiver is also in charge of leaving lobby and joining the game
        elif msg['MessageType'] == 'StartInstance':
            # Log to chat
            self.chat_pad.addstr(self.curr_chatln-1, 0, f'Lobby full. Joining the game...')
            self.cursor_increment()

            # Connect socket
            addr = (msg['Data']['Server'], msg['Data']['Port'])
            nonce = msg['Data']['Nonce']
            
            self.join(addr, "JoinInstance", nonce)

        elif msg['MessageType'] == 'JoinInstanceResult':
            # Name (again...) and number (sent later...) are useless info
            # Log to chat and restart if game server couldn't be joined
            if msg['Data']['Result'] == 'Yes':
                self.chat_pad.addstr(self.curr_chatln-1, 0, f'{self.username} has joined the game.')
                self.cursor_increment()
            else:
                # Restart back to lobby
                #curses.wrapper(main) -> threading! :(
                sys.exit(1)

        elif msg['MessageType'] == 'StartGame':
            self.rounds = msg['Data']['Rounds'] # store for later
            # Note that PlayerInfo is redundant (see below)
        
        elif msg['MessageType'] == 'StartRound':
            # Display initialzied board state ( - - - - - )
            self.wordLength = msg['Data']['WordLength']
            self.last_guess = '_'*self.wordLength
            self.result = 'B'*self.wordLength
            self.show_board()
            
            self.round = msg['Data']['Round']
            # Rounds remaining = redundant field; ignore
            
            # Log round and current leaderboard 
            self.p_begin_round(msg['Data']['PlayerInfo'])
        
        elif msg['MessageType'] == 'PromptForGuess':
            # Ignoring wordlength (above)
            self.guess_num = msg['Data']['GuessNumber']
            self.chat_pad.addstr(self.curr_chatln-1, 0, f'Please enter your guess ({self.guess_num}/{self.guesses}).')
            self.cursor_increment()
        
        elif msg['MessageType'] == 'GuessResponse':
            # Alert in chat whether guess was valid
            #if msg['Data']['Accepted'] == 'No': NOTE
            if msg['Data']['Result'] == 'No':
                self.chat_pad.addstr(self.curr_chatln-1, 0, f"INVALID GUESS: The word is {self.wordLength} characters long.")
                self.cursor_increment()
            # Store last_guess to display (receiver thread does not communicate with sender)
            else:
                self.last_guess = msg['Data']['Guess']

        elif msg['MessageType'] == 'GuessResult':
            # Ignore winner duplicate
            self.p_post_guess(msg['Data']['PlayerInfo'])
            
            # Display user's results at the TOP
            self.show_board()

        elif msg['MessageType'] == 'EndRound':
            # rounds remaining = duplicate
            self.p_post_round(msg['Data']['PlayerInfo'])

        elif msg['MessageType'] == 'EndGame':
            self.winner = msg['Data']['WinnerName'] 
            self.p_post_game(msg['Data']['PlayerInfo'])

            # NOTE: Leave game
            sys.exit(1)
    
    def render_msgs(self): # receiver thread

        self.curr_chatln = 1
        while True:
            #cool: self.chat_win.getmaxyx()[0]
           
            # Better than for loop: allows dynamic changing of skt from lobby -> game server
            msg = next(self.skt_msgs)
            
            # display chat messages/new game state 
            self.process_msg(json.loads(msg))

            # Flush to screen
            self.chat_pad.noutrefresh(max(0, self.curr_chatln-(self.H-6-int(self.H/6))),0,int(self.H/6),0,self.H-6,self.W)
            
    # Waits for user to enter a msg and then delivers to server
    def await_input(self): # sender thread
        
        h = 1
        w = self.W-10 # would prefer terminal width
        # tl means top-left
        tl_y = self.H+3; tl_x = 0+20 # pad by 20
        
        while True:
            rectangle(self.stdscr, tl_y-1, tl_x, tl_y+1, int(w/2))
            self.box = Textbox(self.editwin)
           
            self.stdscr.noutrefresh()
            self.editwin.noutrefresh()

            # Collect user input (following enter keystroke)
            self.box.edit(self._search_validator)
            msg = self.box.gather().strip().lower()
            
            # Send msg across socket
            msg_json = {'MessageType': self.mode.title(), 'Data': {'Name': self.username, self.mode.title() if self.mode=='guess' else 'Text': msg}}
            
            # Strategy: global variable for skt shared among threads
            try:
                utils.send_nl_message(skt, utils.encode_object(msg_json))
            except: # Broken pipe, lost connection
                pass
            
            self.editwin.clear()


def main(stdscr):
    WordleClient(sys.argv[1], 'student13.cse.nd.edu', 4100, stdscr)

if __name__ == '__main__':
    curses.wrapper(main)

