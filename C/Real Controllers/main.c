// Single/Double Loop Controller Design Switch

#define SINGLE_LOOP
// #define DOUBLE_LOOP

/*
 * Copyright (c) 2015 Prof Garbini
 * Modified 2020 James Muir
 * 
 * Double Loop Controller for SEA
 */

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include "TimerIRQ.h"
#include "ctable2.h"
#include "Encoder.h"
#include <unistd.h>
#include "matlabfiles.h"
#include "math.h"

#define VDAmax +7.5 // max D/A converter voltage: V
#define VDAmin -7.5 // min D/A converter voltage: V
#define Tmax +0.5   // max motor output torque: N-m
#define Tmin -0.5   // min motor output torque: N-m

#define ntot 5000 // number of data points to save

extern NiFpga_Session myrio_session;

typedef struct
{  // segment for position profile
    double xfa;
    double v;
    double a;
    double d;
} seg;

typedef struct
{                                 // Resources for the timer thread
    NiFpga_IrqContext irqContext; // IRQ context reserved by Irq_ReserveContext()
    table *a_table;               // table
    seg *profile;                 // profile
    int nseg;                     // number of segments in profile
    NiFpga_Bool irqThreadRdy;     // IRQ thread ready flag
} ThreadResource;

struct biquad
{ // second-order section
    double b0;
    double b1;
    double b2; // numerator
    double a0;
    double a1;
    double a2; // denominator
    double x0;
    double x1;
    double x2; // input
    double y1;
    double y2; // output
};

#ifdef SINGLE_LOOP
#include "single_loop_controller.h"
#endif /* SINGLE_LOOP */

#ifdef DOUBLE_LOOP
#include "double_loop_controller.h"
#endif /* DOUBLE_LOOP */

// Prototypes
void *Timer_Irq_Thread(void *resource);
double cascade(double xin, struct biquad *fa, int ns, double ymin, double ymax);
double pos(MyRio_Encoder *channel);
double diff(MyRio_Encoder *ch0, MyRio_Encoder *ch1, double tpr0, double tpr1);
//TODO: check if Sramps and conC_Encoder_initialize prototype is needed
int Sramps(seg *segs, int nseg, int *iseg, int *itime, double T, double *xa);
NiFpga_Status conC_Encoder_initialize(NiFpga_Session myrio_session, MyRio_Encoder *encCp, int iE);


/*  This Timer Thread controls the motor and acquires data */
void *Timer_Irq_Thread(void *resource)
{

    ThreadResource *threadResource = (ThreadResource *)resource;
    uint32_t irqAssert = 0;
    MATFILE *mf;
    MyRio_Aio CI0, CO0;
    MyRio_Encoder encC0;
    MyRio_Encoder encC1;
    double P2Ref[ntot], P2Act[ntot], TM[ntot], P1Act[ntot];
    #ifdef DOUBLE_LOOP
    double TsRef[ntot], TsAct[ntot];
    #endif /* DOUBLE_LOOP */

    int isave = 0;
    double VDAout;
    int j, err;
    double t[ntot]; // time vector

    double *P2_ref = &((threadResource->a_table + 0)->value); //Convenient pointer names for the table values
    double *P2_act = &((threadResource->a_table + 1)->value);
    double *VDA_out_mV = &((threadResource->a_table + 2)->value); // mV
    double *P1_act = &((threadResource->a_table + 3)->value);
    #ifdef DOUBLE_LOOP
    double *Ts_ref = &((threadResource->a_table + 4)->value);
    double *Ts_act = &((threadResource->a_table + 5)->value);
    #endif /* DOUBLE_LOOP */

    int iseg = -1, itime = -1, nsamp, done;
    seg *mySegs = threadResource->profile;
    int nseg = threadResource->nseg;

    double T; // time (s)
    double P2_err; // output position error

    #ifdef DOUBLE_LOOP
    double Ts_err; // torque error
    #endif /* DOUBLE_LOOP */

    //  Initialize interfaces before allowing IRQ
    AIO_initialize(&CI0, &CO0);                        // initialize analog I/O
    Aio_Write(&CO0, 0.0);                              // stop motor
    conC_Encoder_initialize(myrio_session, &encC0, 0); // initialize encoder 0
    conC_Encoder_initialize(myrio_session, &encC1, 1); // initialize encoder 1

    // printf("timeoutValue %g\n",(double)timeoutValue); // debug output

    while (threadResource->irqThreadRdy)
    {
        T = timeoutValue / 1.e6; // sample period - s (BTI length)
        Irq_Wait(threadResource->irqContext,
                 TIMERIRQNO, // wait for IRQ to assert or signal sent
                 &irqAssert,
                 (NiFpga_Bool *)&(threadResource->irqThreadRdy));
        NiFpga_WriteU32(myrio_session,
                        IRQTIMERWRITE,
                        timeoutValue); /* write timer register */
        NiFpga_WriteBool(myrio_session,
                         IRQTIMERSETTIME,
                         NiFpga_True); /* toggle to reset the timer */

        if (irqAssert)
        {
            // compute the next profile value
            done = Sramps(mySegs,
                          nseg,
                          &iseg,
                          &itime,
                          T,
                          P2_ref); // reference position (revs)
            if (done)
                nsamp = done;

            *P2_act = pos(&encC1) / BDI_per_rev;  // current position BDI to (revs)
            *P1_act = pos(&encC0) / BDI_per_rev;  // current position BDI to (revs)

            #ifdef SINGLE_LOOP
            // compute error signal
            P2_err = (*P2_ref - *P2_act) * 2 * M_PI; // error signal revs to (radians)

            /* compute control signal */
            VDAout = cascade(P2_err,
                             single_loop_controller,
                             single_loop_controller_ns,
                             VDAmin,
                             VDAmax);       // Vda
            *VDA_out_mV = trunc(1000. * VDAout); // table show values
            #endif /* SINGLE_LOOP */

            #ifdef DOUBLE_LOOP
            // outer loop (position control)
            P2_err = (*P2_ref - *P2_act) * 2 * M_PI; // error signal revs to (radians)

            *Ts_ref = cascade(P2_err, outer_loop_controller, outer_loop_controller_ns, Tmax, Tmin);

            // inner loop (torque control)
            *Ts_act = diff(&encC0, &encC1, BDI_per_rev, BDI_per_rev) * 2 * M_PI * Krot;      // current output torque (N-m)
            Ts_err = (*Ts_act - *Ts_ref); // torque error (N-m)

            VDAout = cascade(Ts_err,
                             inner_loop_controller,
                             inner_loop_controller_ns,
                             VDAmin,
                             VDAmax);       // Vda
            *VDA_out_mV = trunc(1000. * VDAout); // table show values
            #endif /* DOUBLE_LOOP */

            Aio_Write(&CO0, VDAout);        // output control value

            /* save data */
            if (isave < ntot)
            {
                P2Act[isave] = *P2_act * 2 * M_PI; // radians
                P2Ref[isave] = *P2_ref * 2 * M_PI;  // radians
                TM[isave] = VDAout * Kt * Kvi;  // N-m	--- NEW AMPLIFIER
                P1Act[isave] = *P1_act * 2 * M_PI; // rad
                #ifdef DOUBLE_LOOP
                TsRef[isave] = *Ts_ref; // N-m
                TsAct[isave] = *Ts_act; // N-m
                #endif /* DOUBLE_LOOP */
                isave++;
            }
            Irq_Acknowledge(irqAssert); /* Acknowledge the IRQ(s) the assertion. */
        }
    }
    Aio_Write(&CO0, 0.0); // stop motor
    // printf("nsamp: %g\n",(double) nsamp); // debug print statement
    //---Save Data to a .mat file in MKS units
    printf("Write MATLAB file\n");
    mf = openmatfile("Lab8.mat", &err);
    if (!mf)
        printf("Can't open mat file error %d\n", err);
    for (j = 0; j < nsamp; j++)
        t[j] = (double)j * T;
    err = matfile_addmatrix(mf, "time", t, nsamp, 1, 0);
    err = matfile_addmatrix(mf, "controller_segments", (double *)mySegs, nseg, 4, 0);

    err = matfile_addstring(mf, "name", "SEA Team");
    err = matfile_addmatrix(mf, "reference_position", P2Ref, nsamp, 1, 0);
    err = matfile_addmatrix(mf, "actual_position", P2Act, nsamp, 1, 0);
    err = matfile_addmatrix(mf, "motor_torque", TM, nsamp, 1, 0);
    err = matfile_addmatrix(mf, "motor_position", P1Act, nsamp, 1, 0);
    #ifdef SINGLE_LOOP
    err = matfile_addmatrix(mf, "single_loop_controller", (double *)single_loop_controller, 6, 1, 0); // TODO: make sure 6 is the right size for my controller
    #endif /* SINGLE_LOOP */
    #ifdef DOUBLE_LOOP
    err = matfile_addmatrix(mf, "reference_spring_torque", TsRef, nsamp, 1, 0);
    err = matfile_addmatrix(mf, "actual_spring_torque", TsAct, nsamp, 1, 0);
    err = matfile_addmatrix(mf, "inner_loop_controller", (double *)inner_loop_controller, 6, 1, 0); // TODO: make sure 6 is the right size for my controller
    err = matfile_addmatrix(mf, "outer_loop_controller", (double *)outer_loop_controller, 6, 1, 0); // TODO: make sure 6 is the right size for my controller
    #endif /* DOUBLE_LOOP */
    err = matfile_addmatrix(mf, "T", &T, 1, 1, 0);
    matfile_close(mf);

    pthread_exit(NULL); /* Exit the new thread. */
    return NULL;
}

/*--------------------------------------------------------------
 Function cascade
 Purpose:		implements cascade of biquad sections
 Parameters:
 xin -			current input to the cascade
 fa -			the  array of type biquad, each element
 	 	 	 	contains the filter coefficients, input
 	 	 	 	and output history variables
 ns -			number of biquad sections
 ymin -			minimum output saturation limit
 ymax -			maximum output saturation limit
 Returns:		Current value, y0, of the final biquad
 *--------------------------------------------------------------*/
#define SATURATE(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
double cascade(double xin, struct biquad *fa, int ns, double ymin, double ymax)
{

    char i; /* biquad section index */
    double y0;
    struct biquad *f; /* declare a pointer to a variable of type biquad */

    f = fa;   // point to  the first biquad
    y0 = xin; // pass the input to the first biquad in the cascade
    for (i = 0; i < ns; i++)
    {               // loop through the "ns" biquads
        f->x0 = y0; // pass the output to the next biquad
        y0 = (f->b0 * f->x0 + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2) / f->a0;
        if (i == ns - 1)
            y0 = SATURATE(y0, ymin, ymax);
        f->x2 = f->x1; // Update the input history of this biquad
        f->x1 = f->x0;
        f->y2 = f->y1; // Update the output history of this biquad
        f->y1 = y0;
        f++; // point to the next biquad
    }
    return y0; // return the output of the cascade
}

/*--------------------------------------------------------------
 Function pos
	Purpose		Read the encoder counter, compute the current
			estimate of the motor position.
	Parameters:	encoder channel
	Returns: 	encoder position (BDI)
*--------------------------------------------------------------*/
double pos(MyRio_Encoder *channel)
{
    int currentP; // current position (sizeof(int) = 4 bytes

    static int startP;
    static int first = 1; // first time calling pos();

    int deltaP;
    double position; // position estimate

    currentP = Encoder_Counter(channel);
    // initialization
    if (first)
    {
        startP = currentP;
        first = 0;
    };

    deltaP = currentP - startP;
    position = (double)deltaP; // BDI - displacement from starting position
    return position;
}

/*--------------------------------------------------------------
 Function diff
	Purpose		Return the difference between the angles of the two shafts
	Parameters:	
        ch0 - encoder channel 0
        ch1 - encoder channel 1
        tpr0 - ticks per revolution for encoder 0
        tpr1 - ticks per revolution for encoder 1
	Returns: 	encoder position (BDI)
*--------------------------------------------------------------*/
double diff(MyRio_Encoder *ch0, MyRio_Encoder *ch1, double tpr0, double tpr1)
{
    return ((pos(ch1) / tpr1) - (pos(ch0) / tpr0)); // (rev)
}

int main(int argc, char **argv)
{
    int32_t status;
    MyRio_IrqTimer irqTimer0;
    ThreadResource irqThread0;
    pthread_t thread;
    uint32_t timeoutValue; /* time interval - us */
    double bti = 0.5;
    double vmax, amax, dwell;
    int nseg;

    char *Table_Title = "Position Controller";
 
    table my_table[] = {
        {"P2_ref: rev  ", 0, 0.0}, // output pulley reference position
        {"P2_act: rev  ", 0, 0.0}, // output pulley actual position
        {"VDA_out: mV  ", 0, 0.0}, // myRIO output voltage
        {"P1_act: rev  ", 0, 0.0} // motor pulley actual position
        #ifdef DOUBLE_LOOP
        ,{"Ts_ref: N-m  ", 0, 0.0}, // spring reference torque
        {"Ts_act: N-m  ", 0, 0.0}  // spring actual torque
        #endif /* DOUBLE_LOOP */
        };
    #ifdef SINGLE_LOOP
    int table_entries = 4;
    #endif /* SINGLE_LOOP */
    #ifdef DOUBLE_LOOP
    int table_entries = 6;
    #endif /* DOUBLE_LOOP */

    vmax = 10.;
    amax = 10.;
    dwell = 5.0;
    seg mySegs[4] = {// revolutions
                     {1.0, vmax, amax, dwell},
                     {0.0, vmax, amax, dwell},
                     {-1.0, vmax, amax, dwell},
                     {0.0, vmax, amax, dwell}};
    nseg = 4;

    /*  registers corresponding to the IRQ channel     */
    irqTimer0.timerWrite = IRQTIMERWRITE;
    irqTimer0.timerSet = IRQTIMERSETTIME;
    timeoutValue = bti * 1000.;

    /* Open the myRIO NiFpga Session. */
    status = MyRio_Open();
    if (MyRio_IsNotSuccess(status))
    {
        return status;
    }

    /* Configure the timer IRQ. */
    status = Irq_RegisterTimerIrq(&irqTimer0,
                                  &irqThread0.irqContext,
                                  timeoutValue);
    if (status != NiMyrio_Status_Success)
    { /* Terminate the process if it is unsuccessful */
        printf("Status: %d, Configuration of Timer IRQ failed.", status);
        return status;
    }

    /* Create new thread to catch the timer IRQ */
    irqThread0.irqThreadRdy = NiFpga_True; /* Set the indicator to allow the new thread.*/
    irqThread0.a_table = my_table;
    irqThread0.profile = mySegs;
    irqThread0.nseg = nseg;
    status = pthread_create(&thread,
                            NULL,
                            Timer_Irq_Thread, // name of timer thread
                            &irqThread0);     // thread resource
    if (status != NiMyrio_Status_Success)
    {
        printf("Status: %d, Failed to create irq thread!", status);
        return status;
    }

    ctable2(Table_Title, my_table, table_entries); // start the table editor

    //	All Done.  Terminate Timer Thread
    irqThread0.irqThreadRdy = NiFpga_False; /* Set  indicator to end the timer thread.*/
    pthread_join(thread, NULL);             /* Wait for the end of the IRQ thread. */
    printf("Timer thread ends.\n");
    printf_lcd("Timer thread ends.\n");

    // Disable timer interrupt, so you can configure this I/O next time.
    status = Irq_UnregisterTimerIrq(&irqTimer0,
                                    irqThread0.irqContext);
    if (status != NiMyrio_Status_Success)
    {
        printf("Status: %d\nClear Timer IRQ failed.\n", status);
        return status;
    }
    status = MyRio_Close(); // Close the myRIO NiFpga Session.
    return status;          // Returns 0 if successful.
}
