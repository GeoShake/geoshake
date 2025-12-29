#ifndef SEISMIC_DISCRIMINATOR_H
#define SEISMIC_DISCRIMINATOR_H

#include <Arduino.h>

/*
 * RECURSIVE STA/LTA ALGORITHM WITH POLARIZATION ANALYSIS
 * ======================================================
 * * Theory:
 * 1. We track energy on Vertical (Z) and Horizontal (H = sqrt(X^2+Y^2)) channels.
 * 2. We use recursive filters (IIR) for STA and LTA to save RAM.
 * STA_new = (val * C_STA) + (STA_old * (1 - C_STA))
 * 3. A "Trigger" happens when STA / LTA > threshold.
 * 4. P-Wave Logic: Trigger occurs on Z-axis first.
 * 5. S-Wave Logic: Energy shifts to H-axis after P-trigger window.
 */

class SeismicDiscriminator {
private:
    // Filter Coefficients (pre-calculated for 100Hz-200Hz sampling)
    // STA window ~0.5s, LTA window ~10s
    const float C_STA = 0.1;   
    const float C_LTA = 0.01; 
    
    // Thresholds
    const float TRIGGER_RATIO = 2.5;     // STA/LTA ratio to trigger event
    const float S_WAVE_RATIO = 1.2;      // Horizontal/Vertical energy ratio to declare S-wave
    const unsigned long MIN_P_DURATION = 50; // ms
    
    // State variables
    float staZ = 0.0, ltaZ = 0.0;
    float staH = 0.0, ltaH = 0.0;
    
    unsigned long pWaveStartTime = 0;
    bool pWaveActive = false;
    bool sWaveActive = false;
    
    // Distance Estimation
    unsigned long sWaveArrivalTime = 0;
    
    enum State {
        STATE_IDLE,
        STATE_P_WAVE,
        STATE_S_WAVE_SEARCH,
        STATE_COOLDOWN
    };
    State currentState = STATE_IDLE;

public:
    struct Result {
        String waveType;     // "None", "P-Wave", "S-Wave", "Surface"
        float confidence;    // 0.0 to 1.0
        float verticalRatio; // STA/LTA Z
        float horizontalRatio; // STA/LTA H
        float estimatedDistanceKm; // Based on S-P time
    };

    SeismicDiscriminator() {
        // Initialize baselines to avoid startup triggers
        staZ = ltaZ = 0.002; // Small noise floor
        staH = ltaH = 0.002;
    }

    // Call this with calibrated accelerometer data (m/s^2 or g, gravity removed)
    Result update(float accE, float accN, float accU) {
        Result res;
        res.estimatedDistanceKm = 0.0;
        
        // 1. Calculate Absolute Energies
        float absZ = abs(accU);
        float absH = sqrt(accE * accE + accN * accN);
        
        // 2. Update Recursive STA/LTA
        // This simulates a rolling window without storing arrays
        staZ = (absZ * C_STA) + (staZ * (1.0 - C_STA));
        ltaZ = (absZ * C_LTA) + (ltaZ * (1.0 - C_LTA));
        
        staH = (absH * C_STA) + (staH * (1.0 - C_STA));
        ltaH = (absH * C_LTA) + (ltaH * (1.0 - C_LTA));
        
        // Avoid divide by zero
        if (ltaZ < 0.0001) ltaZ = 0.0001;
        if (ltaH < 0.0001) ltaH = 0.0001;
        
        float ratioZ = staZ / ltaZ;
        float ratioH = staH / ltaH;
        
        res.verticalRatio = ratioZ;
        res.horizontalRatio = ratioH;
        
        unsigned long now = millis();

        // 3. State Machine for Discrimination
        switch (currentState) {
            case STATE_IDLE:
                res.waveType = "None";
                res.confidence = 0.0;
                
                // P-Wave Trigger: High vertical energy relative to background
                if (ratioZ > TRIGGER_RATIO) {
                    currentState = STATE_P_WAVE;
                    pWaveStartTime = now;
                    pWaveActive = true;
                    res.waveType = "P-Wave (Onset)";
                    res.confidence = 0.8;
                }
                break;

            case STATE_P_WAVE:
                res.waveType = "P-Wave";
                res.confidence = 0.9;
                
                // Transition to S-Wave Search if sufficient time passed (P coda)
                // or if Horizontal energy starts spiking
                if (now - pWaveStartTime > 200) { 
                    currentState = STATE_S_WAVE_SEARCH;
                }
                
                // Timeout check (false alarm?)
                if (ratioZ < 1.5 && (now - pWaveStartTime > 2000)) {
                    currentState = STATE_COOLDOWN;
                }
                break;

            case STATE_S_WAVE_SEARCH:
                // We are looking for the S-arrival.
                // Physics: S-wave is shear, dominates Horizontal components.
                // Logic: If Horizontal Ratio spikes OR Horizontal Raw > Vertical Raw * 1.5
                
                if (ratioH > TRIGGER_RATIO || (staH > staZ * S_WAVE_RATIO)) {
                    currentState = STATE_COOLDOWN; // Event fully characterized
                    sWaveArrivalTime = now;
                    
                    // S-P Time Calculation (Omori Formula approximation)
                    // Distance (km) ~ 8 * (Ts - Tp)
                    float deltaT = (sWaveArrivalTime - pWaveStartTime) / 1000.0;
                    res.estimatedDistanceKm = 8.0 * deltaT;
                    
                    res.waveType = "S-Wave Arrival";
                    res.confidence = 0.95;
                } else {
                    res.waveType = "P-Wave (Coda)";
                }
                
                // Timeout
                if (now - pWaveStartTime > 15000) {
                    currentState = STATE_COOLDOWN;
                }
                break;

            case STATE_COOLDOWN:
                res.waveType = "Event Coda";
                // Wait for LTA to catch up to STA (stabilization)
                if (ratioZ < 1.2 && ratioH < 1.2) {
                    currentState = STATE_IDLE;
                    pWaveActive = false;
                }
                break;
        }

        return res;
    }
    
    void reset() {
        currentState = STATE_IDLE;
        // Reset to noise floor (same as constructor) to avoid startup triggers
        staZ = ltaZ = 0.002;
        staH = ltaH = 0.002;
        pWaveStartTime = 0;
        sWaveArrivalTime = 0;
        pWaveActive = false;
        sWaveActive = false;
    }
};

#endif