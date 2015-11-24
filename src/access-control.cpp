#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <sstream>
#include <thread>
#include <ctime>
#include <chrono>
#include <string>

#include <grove.h>
#include <jhd1313m1.h>
#include "biss0001.h"

#include "../lib/restclient-cpp/include/restclient-cpp/restclient.h"
#include "../lib/crow/crow_all.h"
#include "../src/html.h"

using namespace std;

bool countdownStarted = false;
bool disarmed = false;
bool alarmTriggered = false;

// The time that the motion was detected
chrono::time_point<chrono::system_clock> detectTime, currentTime, disarmTime;

string access_code() {
  if (!getenv("CODE")) {
    return "4321";
  } else {
    return getenv("CODE");
  }
}

void notify(std::string message) {
  if (!getenv("SERVER") || !getenv("AUTH_TOKEN")) {
    return;
  }

  time_t now = std::time(NULL);
  char mbstr[sizeof "2011-10-08T07:07:09Z"];
  strftime(mbstr, sizeof(mbstr), "%FT%TZ", localtime(&now));

  stringstream payload;
  payload << "{\"state\":";
  payload << "\"" << message << " " << mbstr << "\"}";

  RestClient::headermap headers;
  headers["X-Auth-Token"] = getenv("AUTH_TOKEN");

  RestClient::response r = RestClient::put(getenv("SERVER"), "text/json", payload.str(), headers);
  cout << "Datastore called. Result:" << r.code << endl;
  cout << r.body << endl;
}

// The hardware devices that the example is going to connect to
struct Devices
{
  upm::Jhd1313m1* screen;
  upm::BISS0001* motion;

  Devices(){
  };

  // Initialization function
  void init() {
    // screen connected to the default I2C bus
    screen = new upm::Jhd1313m1(0);

    // motion sensor on digital D4
    motion = new upm::BISS0001(4);
  };

  // Cleanup on exit
  void cleanup() {
    delete screen;
    delete motion;
  }

  // Display a message on the LCD
  void message(const string& input, const size_t color = 0x0000ff) {
  cout << input << std::endl;
    size_t red   = (color & 0xff0000) >> 16;
    size_t green = (color & 0x00ff00) >> 8;
    size_t blue  = (color & 0x0000ff);

    string text(input);
    text.resize(16, ' ');

    screen->setCursor(0,0);
    screen->write(text);
    screen->setColor(red, green, blue);
  }

  void trigger_alarm() {
    alarmTriggered = true;
    string msg = "Alarm triggered!";
    message(msg, 0xff00ff);
    notify(msg);
  }

  void start_alarm_countdown() {
    countdownStarted = true;
    string msg = "Person detected";
    message(msg, 0xff00ff);
    notify(msg);
  }

  void detect() {
    chrono::duration<double> elapsed;
    currentTime = chrono::system_clock::now();

    if (alarmTriggered) {
      elapsed = currentTime - detectTime;

      if (elapsed.count() > 120) {
        reset();
      }
    } else if (disarmed) {
      elapsed = currentTime - disarmTime;

      if (elapsed.count() > 120) {
        reset();
      }
    } else if (countdownStarted) {
      elapsed = currentTime - detectTime;

      if (elapsed.count() > 30) {
        trigger_alarm();
      }
    } else if (motion->value()) {
      detectTime = currentTime;
      start_alarm_countdown();
    } else {
      message("Monitoring...");
    }
  }

  void disarm() {
    disarmTime = chrono::system_clock::now();
    disarmed = true;
    countdownStarted = false;
    alarmTriggered = false;
  }

  void reset() {
    disarmed = false;
    countdownStarted = false;
    alarmTriggered = false;
  }
};

// Function called by worker thread for device communication
void runner(Devices& devices) {
  for (;;) {
    devices.detect();
    usleep(500);
  }
}

Devices devices;

// Exit handler for program
void exit_handler(int param)
{
  devices.cleanup();
  exit(1);
}

// The main function for the example program
int main() {
  // Handles ctrl-c or other orderly exits
  signal(SIGINT, exit_handler);

  // check that we are running on Galileo or Edison
  mraa_platform_t platform = mraa_get_platform_type();
  if ((platform != MRAA_INTEL_GALILEO_GEN1) &&
    (platform != MRAA_INTEL_GALILEO_GEN2) &&
    (platform != MRAA_INTEL_EDISON_FAB_C)) {
    std::cerr << "ERROR: Unsupported platform" << std::endl;
    return MRAA_ERROR_INVALID_PLATFORM;
  }

  // create and initialize UPM devices
  devices.init();

  // start worker thread for device communication
  std::thread t1(runner, std::ref(devices));

  // define web server & routes
  crow::SimpleApp app;

  CROW_ROUTE(app, "/")
  ([]() {
    std::stringstream text;
    text << index_html;
    return text.str();
  });

  CROW_ROUTE(app, "/alarm")
  ([](const crow::request& req) {
    if(req.url_params.get("code") != nullptr) {
      if (access_code() == req.url_params.get("code")) {
        devices.disarm();
      } else {
        notify("invalid code");
      }
    }

    return crow::response("OK");
  });

  // start web server
  app.port(3000).multithreaded().run();

  // wait forever for the thread to exit
  t1.join();

  return MRAA_SUCCESS;
}
