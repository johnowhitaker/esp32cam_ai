# esp32cam_ai

Demo of adding some AI to a project via an ESP32-cam module sending images off to Gemini flash 8B for classification.

The ESP also serves up a hacky debug webpage so you can edit the prompt and interval, and test capturing images + getting classifications. 

![Screenshot 2024-12-24 071537](https://github.com/user-attachments/assets/4e4dfe49-145a-497d-9b11-b8cdb0e7d353)

To run:

- Get an ESP-32 CAM board, I used one of [these](https://www.amazon.com/gp/product/B0948ZFTQZ/) but only because I had it on hand, I think these are found in plenty of places under slightly different branding.
- Open the Arduino IDE (I know, sorry) and install the esp32 board info. I went File -> Preferences and added `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json` as an additional board manager URL, if that fails google around for info. I used `AI-Thinker ESP32-CAM` as my board since that seems to match the pinouts etc of mine.
- Add the code from poc.ino and edit it to add your wifi credentials and an API key from https://aistudio.google.com/apikey
- Upload
- Open the serial monitor and hit the reset button. It will print info to serial, but you can also navigate to the IP address it prints out to get the webpage.

Note: This code is mostly written by o1. This kinda works but I'm not wild about it :) If I end up building on this project I'll likely re-write it myself, but I'm sharing this for those who asked for something they can play with.

The website is slow and glitchy (it's running on a tiny microcontroller that is also trying to take pics and send them off for classification - be patient!). But the main functionality seems to work decently.

Let me know if you have issues or make anything fun :) @johnowhitaker on X, gmail, bluesky, etc.
