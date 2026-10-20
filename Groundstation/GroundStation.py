import lib
import time

def main():
    # Initialization
    lib.startGroundTime()
    lib.loadMIB()
    lib.startComms()

    while True:
        # Uplink
        line = lib.readComms()
        if line:
            name = lib.checkComms(line)
            if name:
                tc = lib.createTC(name)
                lib.timestampCommand(tc)
                lib.displayResponse(f"TC created: {name}")
                lib.sendTelecommand(tc)
        lib.monitorForResponse()
 
        # Downlink
        while (raw := lib.receiveTelemetry()) is not None:
            tm = lib.checkTelemetry(raw)
            if tm:
                lib.timestampTelemetry(tm)
                lib.displayTelemetry(tm)
                lib.logTelemetry(tm)
 
        time.sleep(0.05)
    

