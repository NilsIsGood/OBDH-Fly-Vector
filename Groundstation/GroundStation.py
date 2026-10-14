import lib

def main():
    lib.loadMIB()
    lib.startGroundTime()
    lib.startComms()
    lib.readComms()
    lib.checkComms()
    lib.createTC()
