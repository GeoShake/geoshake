GeoShake T1 is a device designed as an Internet of Things (IoT)–based, distributed seismic monitoring station.

Below are the key details about its core mission and how the project works:



1) Core Mission and Purpose



GeoShake T1’s primary mission is to continuously monitor ground motion (seismic activity) at its location in real time and transmit the collected data to a central server—specifically a HiveMQ MQTT Broker—to help form an “Earthquake Monitoring Network.” The device is always on, constantly listening to and analyzing vibrations.



2) Seismic Sensing Capabilities



GeoShake T1 is more than a simple vibration sensor. It is programmed to compute advanced metrics commonly used in seismology:

- PGA (Peak Ground Acceleration): Measures the maximum acceleration of ground motion. Based on the measured instantaneous acceleration values, the firmware can also produce an estimated Richter-scale magnitude range (for example, if PGA ≥ 0.2g, it may estimate an earthquake in the 4.0–5.0 range).
- Arias Intensity: A cumulative measurement used to understand not only the instantaneous shaking strength, but also the total energy and destructive potential of an earthquake.
- Wave Type Classification (P and S Waves): By analyzing whether the shaking is predominantly vertical (Z axis) or horizontal (X–Y axes), the device can distinguish between P-waves (Primary)—the early arrival phase—and S-waves (Secondary)—typically the more damaging phase.




3) Technical Operating Principle



GeoShake T1 uses an ESP32-WROOM microcontroller and four LSM6DSO/LSM6DSOX accelerometer sensors.

- ENU Coordinate System: Following seismology standards, it uses East (E), North (N), and Up (U) axes. It calibrates using gravity (1g) as a reference and isolates seismic motion from the gravity component.
- Four Sensors for Noise Reduction: The firmware reads from multiple sensors and averages their measurements (e.g., (valX1 + valX2) / 2.0) to reduce noise and improve signal quality.
- Sensitivity and Filtering: The sensors operate in a ±2g range at a 208 Hz sampling rate, and the signal is filtered (around ~23 Hz) to reduce environmental noise.




4) Data Reliability and Communication (Offline Mode)



During an earthquake, internet connectivity may be interrupted. GeoShake T1 addresses this with a dedicated Offline Ring Buffer mechanism:

- When the internet/MQTT connection drops, key data (PGA, Arias, axis values) is stored in the device’s memory (RAM).
- Once connectivity is restored, the device switches back to “Online” mode and transmits the buffered historical data to the server in sequence.




5) User Interface and Setup



- Mobile Provisioning: On first boot, the device creates a Wi-Fi access point named “GeoLink-216.” Users connect via a mobile app to configure the device for their home/office Wi-Fi network.
- Visual and Audible Alerts: An onboard LED and buzzer provide status feedback. For example, the LED may blink rapidly while connecting to Wi-Fi, and blink very fast (e.g., every 50 ms) and/or trigger audible alerts when an earthquake event (PGA threshold) is detected.
- Local Web Interface: Optionally, users can open the device’s local IP address to view a simple web page showing live seismic data.
- Open Source + Flexible Adoption: The project is open source—users can either buy a ready-to-use device (plug and play) or build it themselves and join the network by flashing the firmware.


In summary: GeoShake T1 acts as a smart “digital seismograph” for home or office use—analyzing and classifying seismic waves using professional metrics, while minimizing data loss during connectivity interruptions.



Analogy



You can think of GeoShake T1 as a trained guard dog listening to the Earth’s depths. It won’t react to ordinary noise (like a passing truck), but when it senses real danger (P and S waves), it doesn’t just bark (raise an alarm). It also records how big the threat is (PGA/Richter estimate) and how long it lasts (Arias Intensity) into its notebook (the buffer). Even if the “phone lines” go down, it delivers the full report to its owner (the server) once the connection is restored.