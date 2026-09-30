#include <iostream>		// Required for std::cout and std::endl
#include <string> 		// Required for std::string
#include <iostream>		// Required for std::ofstream (file writing)
#include <fstream> 		// Required for iostream


// Class representing PL ICU
class PayloadICU {
private:
	// Payload Instrument's Power Status: On or Off
	bool isPoweredOn = false;
	
public:
	// [A211] Internal power state update
	void Set_Powered_State(bool powerON){
		isPoweredOn = powerON;
		std::cout << "[PL] Power state changed to: " << (isPoweredOn ? "On" : "Off") << std::endl;
	}
	
	// [A111] Simulating receiving a TC command
	std::string Receive_TC(const std::string& rawPacket) {
		std::cout << "[PL] Pack received: " << rawPacket << std::endl;
		return rawPacket;
	}
	
	// [A112] Validates if command can be executed
	bool Verify_TC(const std::string& command){
		if (!isPoweredOn) {
			std::cout << "[PL Error] Command Rejected: Be Advised, Payload is Off" << std::endl;
			return false; // Reject the command if power is Off
		}
		std::cout << "[PL] Command syntax and stat validated" << std::endl;
		return true; // Accept if power is On
	}
	
	// [A113] Verifies simulated execution of a verified command
	bool Execute_TC(std::string& command){
		std::cout << "[PL] Executing action" << command << ". . ." << std::endl;
		// Simulate image sensor taking a picture . . . or something else
		return true;
	}
	
	// [A114] Simulates sending Telemetry back to OBC
	void Send_TM(const std::string& status){
		std::cout << "[PL --> OBC] Telemetry Sent: " << status << std::endl;
		
	}
	
	// [A221] Appends Telemetry Log to a text file, "payload_events.log
	void Log_Payload_Event(const std::string& eventDetails){
		std::ofstream logfile("payload_events.log", std::ios::app); // Open in append mode
		if (logfile.is.open()){
			logfile << "[Log Event] " << eventDetails << std::endl;
			logfile.close(); // Save and close log file . . . theoretically . . . hopefully
			std::cout << "[PL] Event logged to local text file." << std::endl;
			
		}
	}
};


int main(){
	PayloadICU pl; // Instantiates a PayloadICU Object from the æther or something
	
	// Operational Test Cycle
	pl.Set_Powered_State(true);													// 1. System On
	std::string packet = pl.Receive_TC("TAKE IMAGE");							// 2. Receive Command
	
	// 3. Verify Packet before executing
	if (pl.Verify_TC(packet)) {
		pl.Execute_TC(packet);													// 4. Run Command
		pl.Log_Payload_Event("Image captured successfully at OBT t = 100s");	// 5. Write Log
		pl.Send_TM("EXECUTION SUCCESS");										// 6. Send Telemetry Update
	}

	pl.Set_Powered_State(false);													// 7. Power off
	return(0);																	// Terminate program . . . sleep deep
}