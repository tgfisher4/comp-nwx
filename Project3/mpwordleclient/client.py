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

        ''' 3 horiz window panes Top,Middle,Bottom: (1) Game state, (2) Chat/Info, (3) User prompt '''
        self.state_win = curses.newwin(int(self.H/6), self.W, 0, 0)
        self.chat_pad = curses.newpad(self.H, self.W)
        #self.prompt_win = curses.newwin(10, self.W, self.H-6, self.W)


        h = 1
        w = self.W-10 # would prefer terminal width
        # tl means top-left
        tl_y = self.H+3; tl_x = 0+20 # pad by 20
        self.editwin = curses.newwin(h, int(w/4), tl_y, tl_x+1)
        
        # Display current mode and how to toggle
        self.disp_toggle_instr()
        
        # Display board
        self.last_guess = 'WORDLE'
        self.result = 'G'*6
        self.show_board()
        
        self.p_to_color = {}

        # Block for user prompt responses/chat messages
        threading.Thread(target=self.await_input, daemon=True).start() 

        # Print ongoing chat and game feed
        threading.Thread(target=self.render_msgs, daemon=True).start()

        # Refresh UI
        while True:
            time.sleep(0.1)
            stdscr.noutrefresh()
            self.editwin.noutrefresh()
            curses.doupdate()

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
    # Need backspace, enter capabilities
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
    
    def disp_toggle_instr(self):
        if self.mode == 'guess':
            s = "GUESSING." 
            s1 = "Press '>' to chat " # xtra space deletes 's'

            #y,x = self.stdscr.getyx()
            #self.stdscr.move(curses.LINES-1,0)
            #self.stdscr.deleteln()
            #self.stdscr.move(y,x)
            
            self.stdscr.addstr(curses.LINES-1, 0, s, curses.A_BOLD | curses.color_pair(3))
            self.stdscr.addstr(curses.LINES-1, 20+int(self.W/2), s1)

            # Change the > to red
            #self.stdscr.chgat(curses.LINES-1, (s+s1).find('>'), 1, curses.A_BOLD | curses.color_pair(1))
        else:
            s = "CHATTING."
            s1 = "Press '$' to guess"
            self.stdscr.addstr(curses.LINES-1, 0, s, curses.A_BOLD | curses.color_pair(3))
            self.stdscr.addstr(curses.LINES-1, 20+int(self.W/2), s1)

            # Change the $ to green; not working rn
            #self.stdscr.chgat(curses.LINES-1, (s+s1).find('$'), 1, curses.A_BOLD | curses.color_pair(2))
        
        self.stdscr.noutrefresh()
        self.editwin.noutrefresh()
        #curses.doupdate
    
    def show_board(self):
        s = '  '.join(list(self.last_guess))
        self.stdscr.addstr(1, 0, s)
        d = {'G': 2, 'Y' : 4}
        for idx, color in enumerate(self.result):
            if color != 'B':
                self.stdscr.chgat(1, idx*3, curses.A_BOLD | curses.color_pair(d[color]))

    def cursor_increment(self):
        # Increment cursor while keeping in bounds
        self.curr_chatln += 1
        # Run out of room in pad; delete first line
        if self.curr_chatln > self.H-5:
            self.chat_pad.move(0,0)
            self.chat_pad.deleteln()
            self.curr_chatln -= 1
    
    def print_playerInfo(self, infos):
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'-----SCOREBOARD-----')
        for info in infos:
            self.chat_pad.addstr(self.curr_chatln, 0, f'{info["Name"]} (#: {info["Number"]}) has score: {info["Score"]}')
            #self.curr_chatln += 1
            self.cursor_increment()
        self.chat_pad.addstr(self.curr_chatln, 0, f'--------------------')
    
    def print_playerInfoResult(self, infos):
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'-----SCOREBOARD-----')
        for info in infos:
            s = 'Correct' if info['Correct'] == 'Yes' else 'Incorrect'
            self.chat_pad.addstr(self.curr_chatln, 0, f'{info["Name"]} ({s}, #: {info["Number"]}) sent guess at time {info["ReceiptTime"]} with result: {info["Result"]}')
            #self.curr_chatln += 1
            self.cursor_increment()
        self.chat_pad.addstr(self.curr_chatln, 0, f'--------------------')
    
    def print_playerInfoRound(self, infos): # Because we want to pass ScoreEarned and Winner now
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'-----SCOREBOARD-----')
        for info in infos:
            s = 'Won' if info['Winner'] == 'Yes' else 'Lost'
            self.chat_pad.addstr(self.curr_chatln, 0, f'{info["Name"]} ({s}, #: {info["Number"]}) has earned score: {info["ScoreEarned"]}')
            #self.curr_chatln += 1
            self.cursor_increment()
        self.chat_pad.addstr(self.curr_chatln, 0, f'--------------------')

    def process_msg(self, msg):
        # Basic idea: process based on message type

        if msg['MessageType'] == 'JoinResult':
            pass
        elif msg['MessageType'] == 'Chat': # Why is the server going to send a msg with "Chat" type?
            player = msg["Data"]["Name"]
            if not player in self.p_to_color:
                self.p_to_color[player] = len(self.p_to_color)%6+1
            self.chat_pad.addstr(self.curr_chatln-1, 0, f'{player}: {msg["Data"]["Text"]}', curses.A_ITALIC | curses.color_pair(self.p_to_color[player]))
            
        # return bool to determine whether to increment line count

        elif msg['MessageType'] == 'StartInstance':
            addr = (msg['Data']['Server'], msg['Data']['Port'])
            nonce = msg['Data']['Nonce']
            # Q: lock?
            self.join(addr, "JoinInstance", nonce)

        elif msg['MessageType'] == 'StartGame':
            self.rounds = msg['Data']['Rounds']
            players = msg['Data']['PlayerInfo']
        
        elif msg['MessageType'] == 'StartRound':
            self.wordLength = msg['Data']['WordLength']
            self.lastGuess = '_'*self.wordLength
            self.result = 'B'*self.wordLength
            self.show_board()
            # So many of the fields are duplicates
            self.round = 1
            self.print_playerInfo(msg['Data']['PlayerInfo'])
        
        elif msg['MessageType'] == 'PromptForGuess':
            # msg in chatlog; tells you round but unneeded
            pass
        
        elif msg['MessageType'] == 'GuessResponse':
            # Accepted just means guess received
            # Using this to get last guess/name so I don't have to lock shared resource
            self.my_name = msg['Data']['Name']
            self.lastGuess = msg['Data']['Guess']


        elif msg['MessageType'] == 'GuessResult':
            winner = msg['Data']['Winner']
            self.print_playerInfoResult(msg['Data']['PlayerInfo'])
            # Print new guess with corresponding colors on the board
            self.result = list(filter(lambda item: item['Name'] == self.my_name, msg['Data']['PlayerInfo']))[0]['Result']

        elif msg['MessageType'] == 'EndRound':
            self.round += 1
            self.print_playerInfoRound(msg['Data']['PlayerInfo'])

        elif msg['MessageType'] == 'EndGame':
            winner = msg['Data']['WinnerName']
            print_playerInfo(msg['Data']['PlayerInfo'])
    
    def render_msgs(self): # receiver thread

        self.curr_chatln = 1
        while True:
            #H = self.chat_win.getmaxyx()[0] -> prob the way I should be doing this
           
            # If send invalid msg need to decrement curr_chatln
            # for msg in utils.nl_socket_messages(skt):
            msg = next(self.skt_msgs)
            print(str(msg), file=sys.stderr)
            with open('log.txt', mode='a') as f:
                f.write(str(json.loads(msg)))
                f.write("\n")
            # display chat messages/new game state 
            self.process_msg(json.loads(msg))

            # Flushes to screen
            self.chat_pad.noutrefresh(max(0, self.curr_chatln-(self.H-6-int(self.H/6))),0,int(self.H/6),0,self.H-6,self.W)
            #curses.doupdate()
            #time.sleep(0.1)
            #self.curr_chatln += 1
            self.cursor_increment()
            
    def await_input(self): # sender thread
        # Waits for user to enter a msg and then delivers to server
        
        h = 1
        w = self.W-10 # would prefer terminal width
        # tl means top-left
        tl_y = self.H+3; tl_x = 0+20 # pad by 20
        #self.editwin = curses.newwin(h, int(w/4), tl_y, tl_x+1)
        
        while True:
            rectangle(self.stdscr, tl_y-1, tl_x, tl_y+1, int(w/2))
            self.box = Textbox(self.editwin)
           
            self.stdscr.noutrefresh()
            self.editwin.noutrefresh()
            #curses.doupdate()

            # Collect user input (following enter keystroke)
            self.box.edit(self._search_validator)
            msg = self.box.gather().strip().lower() # TODO: make upper?
            
            # Send msg across socket
            msg_json = {'MessageType': self.mode.title(), 'Data': {'Name': self.username, self.mode.title() if self.mode=='guess' else 'Text': msg}}
            
            # Strategy: global variable for skt shared among threads
            #print(skt)
            try:
                utils.send_nl_message(skt, utils.encode_object(msg_json))
            except: # Broken pipe, lost connection
                pass
            
            self.editwin.clear()

        # Waits for next key pressed; IGNORE
        #self.stdscr.getkey()


def main(stdscr):
    WordleClient(sys.argv[1], 'student13.cse.nd.edu', 4100, stdscr)

if __name__ == '__main__':
    curses.wrapper(main)

