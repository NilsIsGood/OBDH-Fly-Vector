#include <iostream>     // Required for std::cout and std::endl
#include <string>       // Required for std::string
#include <fstream>      // Required for std::ofstream (file writing)


// Class representing PL ICU
class PayloadICU {
private:
    // Payload Instrument's Power Status: On or Off
    bool isPoweredOn = false;

public:
    // [A211] Internal power state update
    void Set_Powered_State(bool powerON) {
        isPoweredOn = powerON;
        std::cout << "[PL] Power state changed to: " << (isPoweredOn ? "On" : "Off") << std::endl;
    }

    // [A111] Simulating receiving a TC command
    std::string Receive_TC(const std::string& rawPacket) {
        std::cout << "[PL] Packet received: " << rawPacket << std::endl;
        return rawPacket;
    }

    // [A112] Validates if command can be executed
    bool Verify_TC(const std::string& command) {
        if (!isPoweredOn) {
            std::cout << "[PL Error] Command Rejected: Be Advised, Payload is Off" << std::endl;
            return false;   // Reject the command if power is Off
        }
        if (command.empty()) {
            std::cout << "[PL Error] Command Rejected: Empty packet" << std::endl;
            return false;   // Reject a malformed (empty) packet
        }
        std::cout << "[PL] Command syntax and state validated" << std::endl;
        return true;        // Accept if power is On
    }

    // [A113] Simulated execution of a verified command
    bool Execute_TC(const std::string& command) {
        std::cout << "[PL] Executing action: " << command << " . . ." << std::endl;
        // Simulate image sensor taking a picture . . . or something else
        return true;
    }

    // [A114] Simulates sending Telemetry back to OBC
    void Send_TM(const std::string& status) {
        std::cout << "[PL --> OBC] Telemetry Sent: " << status << std::endl;
    }

    // [A221] Appends Telemetry Log to a text file, "payload_events.log"
    void Log_Payload_Event(const std::string& eventDetails) {
        std::ofstream logfile("payload_events.log", std::ios::app);  // Open in append mode
        if (logfile.is_open()) {
            logfile << "[Log Event] " << eventDetails << std::endl;
            logfile.close();    // Save and close log file
            std::cout << "[PL] Event logged to local text file." << std::endl;
        } else {
            std::cout << "[PL Error] Could not open payload_events.log for writing." << std::endl;
        }
    }
};


int main() {
    PayloadICU pl;  // Instantiates a PayloadICU object

    // Operational Test Cycle
    pl.Set_Powered_State(true);                                     				// 1. System On
    std::string packet = pl.Receive_TC("TAKE IMAGE");               				// 2. Receive Command

    // 3. Verify packet before executing
    if (pl.Verify_TC(packet)) {
        if (pl.Execute_TC(packet)) {                                				// 4. Run Command
            pl.Log_Payload_Event("Image captured successfully at OBT t = 100s");   	// 5. Write Log
            pl.Send_TM("EXECUTION SUCCESS");                        				// 6. Send Telemetry Update
        } else {
            pl.Log_Payload_Event("Execution failed at OBT t = 100s");
            pl.Send_TM("EXECUTION FAILURE");
        }
    }

    pl.Set_Powered_State(false);                                    				// 7. Power off
    return 0;                                                       				// Terminate program . . . sleep, sleep
}
