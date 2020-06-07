//---Inner loop PDF controller for SEA device
//---04-Jun-2020 01:05:20
    char        headerTime[] = "04-Jun-2020 01:05:20";
    int         inner_loop_controller_ns = 1;              // number of sections
    uint32_t    timeoutValue = 5000;      // time interval - us; f_s = 200 Hz
    static	struct	biquad inner_loop_controller[]={   // define the array of floating point biquads
        {1.000000e+00, -9.105210e-01, 0.000000e+00, 1.000000e+00, -3.196563e-01, 0.000000e+00, 0, 0, 0, 0, 0}
        };