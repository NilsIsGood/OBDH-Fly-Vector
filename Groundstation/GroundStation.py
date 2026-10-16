import lib

def main():
    "Initialization"
    lib.startGroundTime()
    lib.loadMIB()
    lib.startComms()

    "Console"
    lib.readComms()
    lib.checkComms()
    lib.createTC()
    lib.timestampCommand("Example Telecommand")
    lib.displayResponse()

    "Uplink"
    lib.sendTelecommand()
    lib.handleSequenceCount()
    lib.monitorForResponse()

    "Downlink"
    lib.receiveTelemetry()
    lib.checkTelemetry()
    lib.timestampTelemetry()
    lib.displayTelemetry()
    lib.logTelemetry()
    

