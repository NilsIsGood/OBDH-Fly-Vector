import time
import json
import queue
import sys

"""
GLOBALS
"""
EXIT_WORDS = {"exit", "quit", "q"}
_mib = {}
_stdin_q = queue.Queue()     # operators input


DEFAULT_MIB = {
    "Example Telecommand": {},
    "Ping": {},
    "Set Mode Safe": {},
}

"""
INITIALISATION
"""
def startGroundTime():
    """
    Starts the ground time for the ground station.
    This function initializes the ground time and sets it to the current system time.
    """
    global _ground_time, _wall_time
    _ground_time = time.monotonic()
    _wall_time = time.time()

def _now() -> float:
    """
    Monotonic ground elapsed time (s); immune to wall-clock jumps.
    """
    return time.monotonic() - _ground_time

def loadMIB(path="mib.json"):
    """
    Load the Mission Information Base (telecommand definitions).
    """
    global _mib
    try:
        with open(path) as f:
            _mib = json.load(f)
    except FileNotFoundError:
        _mib = dict(DEFAULT_MIB)
        print(f"[MIB] {path} not found, using built-in defaults ({len(_mib)} commands)")

def startComms():
    """
    Starts the communications for the ground station.
    This function initializes the communication interfaces and prepares them for data transmission.
    """
    # Implementation for starting communications goes here
    pass

"""
CONSOLE
"""

def readComms():
    """
    This function retrieves incoming data from standard in and processes it accordingly.
    Return a line typed by the operator, or None. Non-blocking.
    """
    try:
        return _stdin_q.get_nowait().strip() or None
    except queue.Empty:
        return None

def checkComms(line):
   """
   Validate operator input against the MIB. Returns the command name or None.
   Typing exit / quit / q shuts the ground station down.
   """
   if line.lower() in EXIT_WORDS: #lower makes lowercase
        print("[GND] shutdown")
        sys.exit(0)

   if line in _mib:
        return line

   print(f"[ERR] unknown command '{line}'")
   return None

def createTC():
    """
    Creates a Telecommand (TC) for the ground station.
    This function generates a new telecommand based on the specified parameters and prepares it for transmission.
    """
    # Implementation for creating a telecommand goes here
    pass

def timestampCommand(telecommand):
    """
    2.4.0.0 Timestamp Command.
    Applies a timestamp to the telecommand using the ground time.
    This function records when the command was created or released for transmission.
    """
    # Implementation for timestamping a command goes here
    pass


def displayResponse(response):
    """
    2.5.0.0 Display Response.
    Displays the response to a command on the console.
    This function formats the response (acknowledgement, rejection or timeout) for the operator.
    """
    # Implementation for displaying a response goes here 
    # maybe this isnt neccesary?
    pass

"""
UPLINK
"""

def sendTelecommand(telecommand):
    """
    3.1.0 Send Telecommand.
    Sends a telecommand over the communications interface.
    This function encodes the telecommand and transmits it to the spacecraft.
    """
    # Implementation for sending a telecommand goes here
    pass
 
 
def handleSequenceCount():
    """
    3.2.0 Handle Sequence Count.
    Manages the telecommand sequence counter.
    This function assigns the next sequence count and handles rollover and resynchronisation.
    """
    # Implementation for handling the sequence count goes here
    pass
 
 
def monitorForResponse(telecommand, timeout=None):
    """
    3.3.0 Monitor for Response.
    Monitors the downlink for the response to a sent telecommand.
    This function matches acknowledgements to the sequence count and flags a timeout if none arrives.
    """
    # Implementation for monitoring for a response goes here
    pass

"""
DOWNLINK
"""

def receiveTelemetry():
    """
    4.1.0 Receive Telemetry.
    Receives raw telemetry from the communications interface.
    This function reads incoming frames or packets and returns them for processing.
    """
    # Implementation for receiving telemetry goes here
    pass
 
 
def checkTelemetry(telemetry):
    """
    4.2.0 Check Telemetry.
    Validates received telemetry.
    This function checks packet integrity (length, checksum, sequence) and decodes it using the MIB.
    """
    # Implementation for checking telemetry goes here
    pass
 
 
def timestampTelemetry(telemetry):
    """
    4.3.0 Timestamp Telemetry.
    Applies a ground reception timestamp to the telemetry.
    This function records the ground time at which the telemetry was received.
    """
    # Implementation for timestamping telemetry goes here
    pass
 
 
def displayTelemetry(telemetry):
    """
    4.4.0 Display Telemetry.
    Displays telemetry on the console.
    This function formats the decoded parameters for the operator.
    """
    # Implementation for displaying telemetry goes here
    pass
 
 
def logTelemetry(telemetry):
    """
    4.5.0 Log Telemetry.
    Writes telemetry to the log.
    This function stores the timestamped telemetry so it can be reviewed or replayed later.
    """
    # Implementation for logging telemetry goes here
    pass
 
