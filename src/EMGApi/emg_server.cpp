/*
 * Copyright (c) 2013-2023  Bernd Porr <mail@berndporr.me.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 */

#include <string.h>
#include <unistd.h>

#include "json_fastcgi_web_api.h"
#include "EMGSensor.h"
#include <jsoncpp/json/json.h>

/**
 * Flag to indicate that we are running.
 * Needed later to quit the idle loop.
 **/
bool mainRunning = true;

/**
 * Handler when the user has pressed ctrl-C
 * send HUP via the kill command.
 **/
void sigHandler(int sig) { 
	if((sig == SIGHUP) || (sig == SIGINT)) {
		mainRunning = false;
	}
}


/** 
 * Sets a signal handler so that you can kill
 * the background process gracefully with:
 * kill -HUP <PID>
 **/
void setHUPHandler() {
	struct sigaction act;
	memset (&act, 0, sizeof (act));
	act.sa_handler = sigHandler;
	if (sigaction (SIGHUP, &act, NULL) < 0) {
		perror ("sigaction");
		exit (-1);
	}
	if (sigaction (SIGINT, &act, NULL) < 0) {
		perror ("sigaction");
		exit (-1);
	}
}


/**
 * Handler which receives the data here just saves
 * the most recent sample with timestamp. Obviously,
 * in a real application the data would be stored
 * in a database and/or triggers events and other things!
 **/
class SENSORfastcgicallback : public SensorCallback {
public:
	std::deque<float> emgBuffer;
	std::deque<long> timeBuffer;
	long t;
	const int maxBufSize = 50;
	float lastValue = 0;

	/**
	 * Callback with the fresh ADC data.
	 * That's where all the internal processing
	 * of the data is happening. Here, we just
	 * convert the raw ADC data to emg_data
	 * and store it in a variable.
	 **/
	virtual void hasSample(float v) {
		lastValue = v;
		emgBuffer.push_back(v);
		if (emgBuffer.size() > maxBufSize) emgBuffer.pop_front();
		// timestamp
		t = getTimeMS();
		timeBuffer.push_back(t);
		if (timeBuffer.size() > maxBufSize) timeBuffer.pop_front();
	}

	void forceemg_data(float temp) {
		for(auto& v:emgBuffer) {
			v = temp;
		}
	}

private:
	static unsigned long getTimeMS() {
                std::chrono::time_point<std::chrono::system_clock> now = 
                        std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        }
};


/**
 * Callback handler which returns data to the
 * nginx server. Here, simply the current emg_data
 * and the timestamp is transmitted to nginx and the
 * javascript application.
 **/
class JSONCGIADCCallback : public JSONCGIHandler::GETCallback {
private:
	/**
	 * Pointer to the ADC event handler because it keeps
	 * the data in this case. In a proper application
	 * that would be probably a database class or a
	 * controller keeping it all together.
	 **/
	SENSORfastcgicallback* sensorfastcgi;

public:
	/**
	 * Constructor: argument is the ADC callback handler
	 * which keeps the data as a simple example.
	 **/
	JSONCGIADCCallback(SENSORfastcgicallback* argSENSORfastcgi) {
		sensorfastcgi = argSENSORfastcgi;
	}

	/**
	 * Gets the data sends it to the webserver.
	 * The callback creates two json entries. One with the
	 * timestamp and one with the emg_data from the sensor.
	 **/
	virtual std::string getJSONString() {
        Json::Value root;
        root["epoch"] = (int)time(NULL);
	root["lastvalue"] = sensorfastcgi->lastValue;
        Json::Value emg_data;
        for(int i = 0; i < sensorfastcgi->emgBuffer.size(); i++) {
        	emg_data[i] = sensorfastcgi->emgBuffer[i];
    	}
        root["emg_data"]  = emg_data;
		Json::Value t;
        for(int i = 0; i < sensorfastcgi->timeBuffer.size(); i++) {
			t[i] = (int)sensorfastcgi->timeBuffer[i];
    	}
		root["time"] = t;
        Json::StreamWriterBuilder builder;
    	const std::string json_file = Json::writeString(builder, root);
        return json_file;
	}
};


/**
 * Callback handler which receives the JSON from jQuery
 **/
class SENSORPOSTCallback : public JSONCGIHandler::POSTCallback {
public:
	SENSORPOSTCallback(SENSORfastcgicallback* argSENSORfastcgi) {
		sensorfastcgi = argSENSORfastcgi;
	}

	/**
	 * As a crude example we force the emg_data readings
	 * to be 20 millivolts for a certain number of timesteps.
	 **/
	virtual void postString(std::string postArg) {
		const auto rawJsonLength = static_cast<int>(postArg.length());
		JSONCPP_STRING err;
		Json::Value root;
		Json::CharReaderBuilder builder;
		const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		if (!reader->parse(postArg.c_str(), postArg.c_str() + rawJsonLength, &root,
				   &err)) {
			std::cout << "error" << std::endl;
			return;
		}
		float temp = root["emg_data"].asFloat();
		std::cerr << root["hello"].asString() << "\n";
		sensorfastcgi->forceemg_data(temp);
	}

	/**
	 * Pointer to the handler which keeps the emg_data
	 **/
	SENSORfastcgicallback* sensorfastcgi;
};
	

// Main program
int main(int argc, char *argv[]) {
	// getting all the ADC related acquistion set up
	EMGSensor sensorcomm;
	SENSORfastcgicallback sensorfastcgicallback;
	sensorcomm.setCallback(&sensorfastcgicallback);

	// Callback handler for data which arrives from the the
	// browser via jquery json post requests:
	SENSORPOSTCallback postCallback(&sensorfastcgicallback);
	
	// Setting up the JSONCGI communication
	// The callback which is called when fastCGI needs data
	// gets a pointer to the SENSOR callback class which
	// contains the samples. Remember this is just a simple
	// example to have access to some data.
	JSONCGIADCCallback fastCGIADCCallback(&sensorfastcgicallback);

	// creating an instance of the fast CGI handler
	JSONCGIHandler jsoncgiHandler;

	// starting the fastCGI handler with the callback and the
	// socket for nginx.
	jsoncgiHandler.start(&fastCGIADCCallback,&postCallback,
							    "/tmp/sensorsocket");

	// starting the data acquisition at the given sampling rate
	sensorcomm.start();

	// catching Ctrl-C or kill -HUP so that we can terminate properly
	setHUPHandler();

	fprintf(stderr,"'%s' up and running.\n",argv[0]);

	// Just do nothing here and sleep. It's all dealt with in threads!
	// At this point for example a GUI could be started such as QT
	// Here, we just wait till the user presses ctrl-c which then
	// sets mainRunning to zero.
	while (mainRunning) sleep(1);

	fprintf(stderr,"'%s' shutting down.\n",argv[0]);

	sensorcomm.stop();
	jsoncgiHandler.stop();

	return 0;
}
