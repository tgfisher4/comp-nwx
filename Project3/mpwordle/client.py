#! /escnfs/home/pbui/pub/pkgsrc/bin/python3
###! /usr/bin/env python3
''' Wordle client '''
''' Jason Brown, Graham Fisher, Tommy Gallagher '''

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

MAX_WORD_LEN = 10

class WordleClient:
    def __init__(self, username, srvr_hostname, lobby_port, stdscr):
        self.username = username
        self.mode = 'guess' # other option is 'guess'; current user mode
        # Open socket and attempt to join server
        self.join((srvr_hostname, lobby_port), 'Join', 'BrownFisherGallagher-Python')

        ''' Curses UI preliminaries ''' 
        self.stdscr = stdscr
        self.stdscr.clear() # Clear terminal
        
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

        ''' 4 window panes Left, Top Right, Bottom Right Left, Bottom Right Right: (1) Game state, (2) Chat/Info, (3) User prompt (4) User input '''

        self.game_over = False
        self.currln = -1
        self.guess_msg = "[GUESS] ('>' to chat)"
        self.chat_msg =  "[CHAT] ('$' to guess)"
        self.deadline = None

        border = 2
        self.n_spaces = 2
        max_state_width = MAX_WORD_LEN + (MAX_WORD_LEN - 1) * self.n_spaces + 1
        self.max_state_width = max_state_width
        self.state_win = curses.newwin(
            curses.LINES - 1 - 2*border,
            max_state_width,
            border,
            border
        )
        edit_win_ht = 1
        editinfo_wd = max(len(self.guess_msg), len(self.chat_msg)) + 4 + 1 # 4 = len("|999")
        self.editinfo_win = curses.newwin(
            edit_win_ht,
            editinfo_wd,
            curses.LINES - 1 - border - edit_win_ht,
            max_state_width + 2*border
        )
        self.edit_win = curses.newwin(
            edit_win_ht,
            curses.COLS - 1 - (max_state_width + editinfo_wd + 4*border),
            curses.LINES - 1 - border - edit_win_ht,
            max_state_width + editinfo_wd + 3*border
        )
        self.chat_win = curses.newwin(
            curses.LINES - 1 - 3*border - edit_win_ht,
            curses.COLS - 1 - (max_state_width + 3*border),
            border,
            max_state_width + 2*border
        )
        self.chat_win.scrollok(True)
 
        # Display board
        self.guesses = 6 # Rule of the real game
        self.d = {
            'B': curses.color_pair(7),
            'G': curses.A_BOLD | curses.color_pair(2),
            'Y' : curses.A_ITALIC | curses.color_pair(4),
        } # color map for guesses

        # Fix WORDLE at top of screen
        wordle = 'WORDLE'
        n_spaces = (self.max_state_width - len(wordle)) // (len(wordle) - 1)
        offset = (self.max_state_width - (len(wordle) + (len(wordle) - 1) * n_spaces)) // 2
        self.state_win.addstr(0, offset, (' '*n_spaces).join(list(wordle)), self.d['G'])
        self.state_win.noutrefresh()

        # Display current mode and how to toggle
        self.disp_toggle_instr()

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
            # Maybe not the safest or most efficient but does the trick
            self.disp_toggle_instr()
            self.edit_win.noutrefresh()
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
            'MessageType': msg_type,
            "Data": data
        }
        utils.send_nl_message(temp_skt, utils.encode_object(msg))
        skt = temp_skt
        self.skt_msgs = utils.nl_socket_messages(skt)

    # Source: https://www.programcreek.com/python/example/106575/curses.textpad
    # Introduces backspace, enter capabilities
    def _search_validator(self, ch):
        """Fix Enter and backspace for textbox.
        Used as an aux function for the textpad.edit method
        """
        if self.game_over: return 32 # Space
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
                        #self.disp_toggle_instr()
                    return 0
                elif c == '>':
                    if self.mode != 'chat':
                        self.mode = 'chat' 
                        #self.disp_toggle_instr()
                    return 0
            return ch

    # Current message type and how to switch UI display 
    def disp_toggle_instr(self):
        if self.game_over: return
        msg = ""
        color = 0
        if self.mode == 'guess':
            msg = self.guess_msg
            color = curses.color_pair(2) # green
        else:
            msg = self.chat_msg
            color = curses.color_pair(1) # red

        if self.deadline:
            msg = f"[{self.deadline - int(time.time()):0>3}|{msg[1:]}"

        self.editinfo_win.erase()
        self.editinfo_win.addstr(0, 0, msg, curses.A_BOLD | color)
        self.editinfo_win.noutrefresh()

    # Display user's pvs guess results UI
    def clear_board(self):
        self.state_win.move(1,0)
        self.state_win.clrtobot()

    def show_board(self):
        s = (' '*self.n_spaces).join(list(self.last_guess))
        offset = (self.max_state_width - (self.wordLength + (self.wordLength - 1) * self.n_spaces)) // 2
        #s += ' '*6
        self.state_win.addstr(2 * self.guess_num + 1, offset, s)
        
        for idx, color in enumerate(self.result):
            self.state_win.chgat(2*self.guess_num + 1, offset + idx*(self.n_spaces+1), self.d[color])
        self.state_win.noutrefresh()

    def log_to_chat(self, string, attr=0):
        for line in string.split('\n'):
            try:
                self.currln += 1
                self.chat_win.addstr(self.currln, 0, line, attr)
            except:
                # At bottom: scroll everything up and keep cursor at bottom
                self.currln -= 1
                self.chat_win.scroll()
                self.chat_win.addstr(self.currln, 0, line, attr)
        self.chat_win.noutrefresh()

    # Game events in chat
    def p_begin_round(self, infos):
        self.log_to_chat(f'Beginning Round {self.round} of {self.rounds}.')
        for i, p in enumerate(sorted(infos, key=lambda p:p['Score'])):
            self.log_to_chat(f'    {i+1}) {p["Name"]}: {p["Score"]} pts')
    
    def p_post_guess(self, infos):
        # Note that we don't use number, receiptTime, or correct (see winner next) 
        self.log_to_chat(f'Guess Results - ')
        max_name_len = max(len(p['Name']) for p in infos)
        for i, p in enumerate(sorted(infos, key=lambda p:p['Number'])):
            result = p['Result'] 
            if p['Name'] == self.username:
                self.result = result
            
            s = ' '.join(list('@'*self.wordLength))
            t = '    '
            s0 = t + f'{p["Name"]:>{max_name_len}}:' + t
            s = s0 + s
            self.log_to_chat(s)

            loc = len(s0)
            for idx, color in enumerate(result):
                self.chat_win.chgat(self.currln, loc+idx*2, self.d[color])
    
    def p_post_round(self, infos):
        self.log_to_chat(f'Round {self.round} of {self.rounds} complete.')
        winners = [ p for p in infos if p['Winner']=='Yes' ]
        s = '    Winners:'
        winner_blurbs = ''
        for w in winners:
            winner_blurbs += f'    {w["Name"]} (+{w["ScoreEarned"]})'
        s += winner_blurbs or '    No one won this round! Better luck next time.'
        self.log_to_chat(s)
        self.clear_board()

    
    def p_post_game(self, infos):
        self.log_to_chat(f'That concludes our game.')
        self.log_to_chat(f'Congratulations to our winner: {self.winner}!')
        self.log_to_chat(f'Final results - ')

        """
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'That concludes our game.')
        self.cursor_increment()
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'Congratulations to our winner: {self.winner}!')
        self.cursor_increment()
        self.chat_pad.addstr(self.curr_chatln-1, 0, f'Final results - ')
        self.cursor_increment()
        """
        for i, p in enumerate(sorted(infos, key=lambda p:p['Score'])):
            self.log_to_chat(f'    {i+1}) {p["Name"]}: {p["Score"]} pts')

    def end_game(self, msg):
        self.log_to_chat(f"\n{msg} Press ^C to quit.")
        self.editinfo_win.clear()
        self.editinfo_win.addstr(0, 0, "[GAME OVER]", curses.A_BOLD | curses.color_pair(7))
        self.editinfo_win.noutrefresh()
        curses.curs_set(0) # Hide cursor
        self.game_over = True

    
    # Receiver thread to act based on msg supplied by server
    def process_msg(self, msg):

        if msg['MessageType'] == 'JoinResult':
            # Receiver in charge of joining so already knows name...nothing to see here.
            # Log to chat and if lobby server couldn't be joined, probably didn't choose unique name. Exiting...
            if msg['Data']['Result'] == 'Yes':
                self.log_to_chat(f'You joined the lobby.')#{self.username} has joined the lobby.')
            else:
                self.end_game('[ERROR] Unable to join the lobby.')

        elif msg['MessageType'] == 'InvalidMessage':
            self.log_to_chat(f"Oops guessing is not allowed yet. You're still in the lobby!")

        # Note that server will never send a msg with "Chat" type under name 'mpwordle'
        elif msg['MessageType'] == 'Chat':
            player = msg["Data"]["Name"]
            if not player in self.p_to_color:
                # New enemy discovered! Remember his color and post his msg.
                self.p_to_color[player] = len(self.p_to_color)%6+1
            self.log_to_chat(f'{player}: {msg["Data"]["Text"]}', curses.A_ITALIC | curses.color_pair(self.p_to_color[player]))
            
        # Note that receiver is also in charge of leaving lobby and joining the game
        elif msg['MessageType'] == 'StartInstance':
            # Log to chat
            self.log_to_chat(f'Lobby full. Joining the game...')

            # Connect socket
            addr = (msg['Data']['Server'], msg['Data']['Port'])
            nonce = msg['Data']['Nonce']
            
            self.join(addr, "JoinInstance", nonce)

        elif msg['MessageType'] == 'JoinInstanceResult':
            # Name (again...) and number (sent later...) are useless info
            # Log to chat and restart if game server couldn't be joined
            if msg['Data']['Result'] == 'Yes':
                self.log_to_chat(f'You joined the game.')
            else:
                # Ideally, we'd like to return the player to the lobby in this case, but trying to call main or something similar gets weird with threading
                #curses.wrapper(main) -> threading! :(
                self.end_game("[ERROR] Unable to join game after leaving the lobby.")

        elif msg['MessageType'] == 'StartGame':
            self.rounds = msg['Data']['Rounds'] # store for later
            self.clear_board()
            # Note that PlayerInfo is redundant (see below)
        
        elif msg['MessageType'] == 'StartRound':
            # Display initialzied board state ( - - - - - )
            self.wordLength = msg['Data']['WordLength']
            self.last_guess = '-'*self.wordLength
            self.result = 'B'*self.wordLength
            self.guess_num = 1
            self.show_board()
            
            self.round = msg['Data']['Round']
            # Rounds remaining = redundant field; ignore
            
            # Log round and current leaderboard 
            self.p_begin_round(msg['Data']['PlayerInfo'])
        
        elif msg['MessageType'] == 'PromptForGuess':
            # Ignoring wordlength (above)
            self.guess_num = msg['Data']['GuessNumber']
            prompt = f'Please enter your guess ({self.guess_num}/{self.guesses}).'
            try:
                self.deadline = msg["Data"]["GuessDeadline"]
                prompt += f' You have {self.deadline - int(time.time())} seconds.'
            except KeyError: pass
            self.log_to_chat(prompt)
        
        elif msg['MessageType'] == 'GuessResponse':
            # Alert in chat whether guess was valid
            if msg['Data']['Accepted'] == 'No':
                self.log_to_chat(f"INVALID GUESS: The word is {self.wordLength} characters long.")
                self.log_to_chat(msg['Error'] + '.')
            # Store last_guess to display (receiver thread does not communicate with sender)
            else:
                self.log_to_chat(f"Guess received. Please wait until instructed to enter next guess.")
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
            self.end_game('The game is now over.')

        elif msg['MessageType'] == 'PlayerLeave':
            leaver = msg['Data']['Name']
            if leaver == self.username:
                self.log_to_chat("You took too long to guess: you've been kicked out of the game!")
                self.end_game('You left the game.')
            else:
                self.log_to_chat(f"{leaver} left the game.")
    
    def render_msgs(self): # receiver thread
        self.curr_chatln = 1
        while True:
            # Better than for loop: allows dynamic changing of skt from lobby -> game server
            try:
                msg = next(self.skt_msgs)
            except StopIteration:
                break
            
            # display chat messages/new game state 
            self.process_msg(json.loads(msg))

    # Waits for user to enter a msg and then delivers to server
    def await_input(self): # sender thread
        while not self.game_over:
            self.box = Textbox(self.edit_win) 
            self.edit_win.noutrefresh()

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
            
            self.edit_win.erase()


def main(stdscr):
    WordleClient(args.name, args.server, args.port, stdscr)

args = None
if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='Connect to an mpwordleserver to play multiplayer wordle on an ncurses TUI.')
    parser.add_argument("-name", help="name to call you during game", required=True)
    parser.add_argument("-server", help="server serving mpwordle", required=True)
    parser.add_argument("-port", help="port serving mpwordle", type=int, required=True)
    args = parser.parse_args()
    curses.wrapper(main)

