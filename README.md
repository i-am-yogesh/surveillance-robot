The robot is powered by an **ESP32-CAM** with the OV3660 camera, connected to an **L298N motor driver** and four 60RPM 12V DC motors. It runs on a 3S (12V) Li-ion battery pack.

The firmware was written in Arduino IDE using the ESP32 board package by Espressif Systems (v3.3.7). It handles the camera streaming, the web interface, and the motor control logic all on the ESP32 itself.

I’ve also shared the circuit design for reference (put it together on a small website I vibe-coded)
<img width="1156" height="717" alt="surveillance_robot_circuit_design" src="https://github.com/user-attachments/assets/ceeb77ba-8d36-4a07-9ac5-2ac1e78fe21b" />
