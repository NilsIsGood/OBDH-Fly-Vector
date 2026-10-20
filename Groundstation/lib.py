import time

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

def loadMIB():
    """
    Loads the MIB (Management Information Base) for the ground station.
    This function reads the MIB file and initializes the necessary data structures.
    """
    # Implementation for loading MIB goes here
    pass

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
    Reads data from the communications interface.
    This function retrieves incoming data from the communication channels and processes it accordingly.
    """
    # Implementation for reading communications goes here
    pass

def checkComms():
    """
    Checks the status of the communications interface.
    This function verifies if the communication channels are active and functioning properly.
    """
    # Implementation for checking communications goes here
    pass

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
 
