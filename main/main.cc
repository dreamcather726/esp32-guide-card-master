#include "rndis_board.h"

extern "C" void app_main() {
    // The Jiuchuan board powers the ML307 through its SAM8108 circuit.
    static RndisBoard modem;
    modem.StartNetwork();
}
