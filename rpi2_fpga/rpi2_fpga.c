#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <linux/types.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <string.h> 
#include <math.h>
#include <assert.h>
#include <pigpio.h>

#define CONFIG_CYCLES 1

#define CFG_DONE        13      // GPIO_13
#define CFG_PROG         5      // GPIO_05
#define CFG_INIT         6      // GPIO_06
#define CFG_DELAY        1

#define SPI_MOSI        10      // GPIO_10
#define SPI_MISO         9      // GPIO_09
#define SPI_CLK         11      // GPIO_11
#define SPI_CE0_N        8      // GPIO_08
#define SPI_CE1_N        7      // GPIO_07

#define SPI_MAX_LENGTH 4096
// #define SPI_MAX_LENGTH 64
static int  spi_fd;

static unsigned int mode = 0 ;
// failed for RPi2: static unsigned long speed = 16000000UL ;
   static unsigned long speed = 16000000UL ;
// static unsigned long speed =  1000000UL ;
// static unsigned long speed =   100000UL ;
// static unsigned long speed =  9500000UL ;
// static unsigned long speed =   500000UL ;

char configBits[4*1024*1024];

void initGPIOs();
void closeGPIOs();
void clearProgramm();
void setProgramm();
char checkDone();
char checkInit();

void __delay_cycles(unsigned long cycles);

int serialConfig(char * buffer, unsigned int length);

void printHelp(){
    printf("Usage : logi_loader -[r|h] <bitfile> \n");
    printf("-r	will put the FPGA in reset state (lower power consumption)\n");
    printf("-h      will print the help \n");
}

void __delay_cycles(unsigned long cycles){
    while(cycles != 0){
        cycles -- ;	
    }
}


static inline unsigned int min(unsigned int a, unsigned int b){
    if(a < b) return a ;
    return b ;
}

int init_spi(void){
    uint32_t spiFlags;
    // refer to http://abyz.co.uk/rpi/pigpio/cif.html#spiOpen
    //             bbbbbb      R           T           nnnn        W          A          ux         px        mm
    // spiFlags = (0 << 16) | (0 << 15) | (0 << 14) | (0 << 10) | (0 << 9) | (0 << 8) | (0 << 5) | (0 << 2) | mode;
    spiFlags = mode; // default mode(0)
    spi_fd = spiOpen(0, speed, spiFlags);
    printf("spi_fd(%d)\n", spi_fd);
    
    gpioSetPullUpDown(SPI_MOSI,  PI_PUD_UP);   // Sets a pull-up.
    gpioSetPullUpDown(SPI_MISO,  PI_PUD_UP);   // Sets a pull-up.
    gpioSetPullUpDown(SPI_CLK,   PI_PUD_UP);   // Sets a pull-up.
    gpioSetPullUpDown(SPI_CE0_N, PI_PUD_UP);   // Sets a pull-up.
    gpioSetPullUpDown(SPI_CE1_N, PI_PUD_UP);   // Sets a pull-up.

    return spi_fd;
}

void initGPIOs()
{
    if (gpioInitialise() < 0)
    {
        fprintf(stderr, "pigpio initialisation failed\n");
        return;
    }
    // export and configure the pin for our usage
    gpioSetMode(CFG_PROG, PI_OUTPUT);
    gpioSetMode(CFG_INIT, PI_INPUT);
    gpioSetMode(CFG_DONE, PI_INPUT);

    gpioSetPullUpDown(CFG_PROG, PI_PUD_UP);   // Sets a pull-up.
    gpioSetPullUpDown(CFG_INIT, PI_PUD_UP);   // Sets a pull-up.
    gpioSetPullUpDown(CFG_DONE, PI_PUD_UP);   // Sets a pull-up.

    return;
}

void closeGPIOs(){
    /* stop DMA, release resources */
    gpioTerminate();
}

void resetFPGA(){
    // set CFG_PROG(0) to switch FPGA to RECONFIG mode
    gpioWrite(CFG_PROG, 0);
    return;
}

int serialConfig(char * buffer, unsigned int length)
{
    unsigned int count = 0;
    unsigned int write_length, write_index ;
    
    gpioWrite(CFG_PROG, 1);
    __delay_cycles(10 * CFG_DELAY);
    gpioWrite(CFG_PROG, 0);
    __delay_cycles(5 * CFG_DELAY);

    while (gpioRead(CFG_INIT) > 0 && count < 900) {
        printf("count(%d) CFG_INIT(%d)\n", count, gpioRead(CFG_INIT));
        count++; // waiting for init pin to go down
    }
    printf("count(%d) CFG_INIT(%d)\n", count, gpioRead(CFG_INIT));
    if (count >= 900) {
        printf("ERROR: FPGA did not answer to prog request, init pin not going low \n");
        gpioWrite(CFG_PROG, 1);
        return -1;
    }

    count = 0;
    __delay_cycles(5 * CFG_DELAY);
    gpioWrite(CFG_PROG, 1);
    while (gpioRead(CFG_INIT) == 0 && count < 256) { // need to find a better way ...
        printf("count(%d) CFG_INIT(%d)\n", count, gpioRead(CFG_INIT));
        count++; // waiting for init pin to go up
    }
    printf("count(%d) CFG_INIT(%d)\n", count, gpioRead(CFG_INIT));
    if (count >= 256) {
        printf("ERROR: FPGA did not answer to prog request, init pin not going high \n");
        return -1;
    }

    count = 0;
    write_length = min(length, SPI_MAX_LENGTH);
    write_index = 0 ;
    while(length > 0){
        if (gpioRead(CFG_INIT) == 0) {
            printf("CRC ERROR: INIT_B should not go low while configuring FPGA\n");
            printf("length(%d) CFG_INIT(%d)\n", length, gpioRead(CFG_INIT));
            return -1;
        }
        if(spiWrite(spi_fd, &buffer[write_index], write_length) < write_length)
        {
            printf("spi write error \n");
        }
        printf ("length(%d) write_length(%d)\n", length, write_length);
        write_index += write_length ;
        length -= write_length ;
        write_length = min(length, SPI_MAX_LENGTH);
    }
   
    // TODO: keep sending SPI CLK if DONE pin is not high
    if (gpioRead(CFG_DONE) == 0) {
        printf("done pin not going high...\n");
    }
    printf("POST_CRC: INIT_B(%d)\n", gpioRead(CFG_INIT));
    count = 0;
    while (gpioRead(CFG_DONE) == 0) {
        // INIT_B is used as h_txrdy_o after configuration
        printf("SCLK(%d)\r", count);
        buffer[0] = 0xFF;
        write_length = 1;
        if(spiWrite(spi_fd, &buffer[0], write_length) < write_length)
        {
            printf("spi write error \n");
        }
        count ++;
    }

    if (gpioRead(CFG_INIT) == 0) {
        printf("%s (%s:%d) POST_CRC ERROR: INIT_B is low\n", __FILE__, __FUNCTION__, __LINE__);
        printf("count(%d) CFG_INIT(%d)\n", count, gpioRead(CFG_INIT));
        return -1;
    }

    return length;
}


int main(int argc, char ** argv){
    unsigned int i ;
    FILE * fr;
    unsigned int size = 0 ;	

    if (argc == 1) {
        printHelp();
        exit(EXIT_FAILURE);
    }

    //parse programm args
    for(i = 1 ; i < argc ; ){
        if(argv[i][0] == '-'){
            switch(argv[i][1]){
                case '\0': 
                    i ++ ;
                    break ;
                case 'r' :
                    resetFPGA(); 
                    closeGPIOs();
                    return 0 ;
                    break ;
                case 'h' :
                    printHelp();
                    return 0 ;
                    break;
                default :
                    printHelp();
                    return 0 ;
                    break ;
            }
        }else{
            //last argument is file to load
            break ;
        }
    }

    
    initGPIOs();

    if(init_spi() < 0){
        printf("cannot open spi bus \n");
        return -1 ;
    }
    fr = fopen (argv[i], "rb");  /* open the file for reading bytes*/
    if(fr < 0){
        printf("cannot open file %s \n", argv[1]);
        return -1 ;	
    }

    memset((void *) configBits, 0xFF, sizeof(configBits));
    size = fread(configBits, sizeof(char), sizeof(configBits), fr);
    printf("bit file size : %d \n", size);
    assert ((size + 5) <= sizeof(configBits));

    //8*5 clock cycle more at the end of config
    if(serialConfig(configBits, size + 5) < 0){
        printf("config error \n");
        exit(EXIT_FAILURE);
    }else{
        printf("config success ! \n");	
    }

    if (spiClose(spi_fd) != 0)
    {
        printf("config error \n");
        return -1;
    }
    closeGPIOs();
    fclose(fr);
    return 0;
}
