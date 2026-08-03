// ============================================================================
// Diag.h — serial diagnostics
// ============================================================================
// Read-only taps on the live pipeline, except the hardware probes, which
// deliberately bypass it to test the wiring underneath. See Diag.cpp.
#pragma once

void diagTraceToggle();      // live pipeline state WITHOUT muting the instrument
void diagServiceTrace();     // call once per sweep
void diagKeyReport();        // learned baseline and travel, per key
void diagCrosstalkProbe();   // how much a held key bleeds into the others
void diagBleedProbe();       // MEASURE the CROSS_BLEED coefficient

void diagIsolationSnapshot();
void diagFaultProbe();       // shorts, dead sensors, jitter, verdict
void diagBridgeProbe();      // proves a sensor short with all columns off
void diagSettleProbe(int row, int col);
