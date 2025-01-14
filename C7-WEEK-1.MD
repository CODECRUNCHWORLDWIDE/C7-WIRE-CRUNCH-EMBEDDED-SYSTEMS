# Course Content

## Topic: Programming Basics
- **Week Dates:** January 13, 2025 - January 17, 2025
- **Unit:** Week 0 - Overview of C7
- **Link to Slides:** [Programming Basics Slides](https://docs.google.com)
- **Link to tinkercad:** [tinkercad.com/joinclass](https://www.tinkercad.com/joinclass)
- **Link to class link tinkercad:** [WRY8PM3BK](https://www.tinkercad.com/joinclass/WRY8PM3BK)



---

Sample text for instructions

### **Sample Code: C**
```c
#include <stdio.h>

// C++ code
//

// Define pin numbers for RGB LED
#define RED_PIN 9    // Pin connected to the red LED
#define BLUE_PIN 10  // Pin connected to the blue LED
#define GREEN_PIN 11 // Pin connected to the green LED

void setup() {
  // Set RGB LED pins as OUTPUT
  pinMode(RED_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  
  // Initialize Serial communication for debugging
  Serial.begin(9600);
  Serial.println("Starting RGB LED fade...");
}

// Declare variables to store RGB color values
int red, blue, green;

void loop() {
  // First color transition: Red to Blue
  red = 255;   // Start with full intensity red
  blue = 0;    // Start with no intensity blue
  green = 0;   // Green remains off
  for (int i = 0; i <= 255; i += 5) { // Increment intensity by 5
    analogWrite(RED_PIN, red - i);    // Gradually decrease red
    analogWrite(BLUE_PIN, blue + i); // Gradually increase blue
    analogWrite(GREEN_PIN, green);   // Keep green constant
    delay(20);                       // Delay for smooth transition
    Serial.println(i);               // Debug: Print current step
  }

  // Second color transition: Blue to Green
  red = 0;    // Red remains off
  blue = 255; // Start with full intensity blue
  green = 0;  // Start with no intensity green
  for (int i = 0; i <= 255; i += 5) { // Increment intensity by 5
    analogWrite(RED_PIN, red);       // Keep red constant
    analogWrite(BLUE_PIN, blue - i); // Gradually decrease blue
    analogWrite(GREEN_PIN, green + i); // Gradually increase green
    delay(20);                       // Delay for smooth transition
  }

  // Third color transition: Green to Red
  red = 0;    // Start with no intensity red
  blue = 0;   // Blue remains off
  green = 255; // Start with full intensity green
  for (int i = 0; i <= 255; i += 5) { // Increment intensity by 5
    analogWrite(RED_PIN, red + i);    // Gradually increase red
    analogWrite(BLUE_PIN, blue);      // Keep blue constant
    analogWrite(GREEN_PIN, green - i); // Gradually decrease green
    delay(20);                        // Delay for smooth transition
  }
}

```

