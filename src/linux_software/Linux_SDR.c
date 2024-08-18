#include <stdio.h>
#include <sys/mman.h> 
#include <fcntl.h> 
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#define _BSD_SOURCE

#define RADIO_TUNER_FAKE_ADC_PINC_OFFSET 0
#define RADIO_TUNER_TUNER_PINC_OFFSET 1
#define RADIO_TUNER_CONTROL_REG_OFFSET 2
#define RADIO_TUNER_TIMER_REG_OFFSET 3
#define RADIO_PERIPH_ADDRESS 0x43c00000

#define SIMPLE_FIFO_ADDRESS 0x43c10000
#define SIMPLE_FIFO_DATA_OFFSET 0
#define SIMPLE_FIFO_COUNT_OFFSET 1

#define PORT 25344
#define UDP_LEN 1026    // bytes

// radio variables
char freq_str[10];
int adc_freq = 0;
int tune_freq = 0;

// data stream variables
char dest_IP[20];
bool stream_on = true;
char udp_data[UDP_LEN];     // each UDP frame is 1026 bytes
uint16_t frame_counter = 0; // 16-bit unsigned UDP frame counter
int FIFO_count = 0;
int word;                   // 32-bit word
int sock;
struct sockaddr_in servaddr;

volatile unsigned int *my_radio;  
volatile unsigned int *my_fifo;

// the below code uses a device called /dev/mem to get a pointer to a physical
// address.  We will use this pointer to read/write the custom peripheral
volatile unsigned int * get_a_pointer(unsigned int phys_addr)
{

	int mem_fd = open("/dev/mem", O_RDWR | O_SYNC); 
	void *map_base = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, phys_addr); 
	volatile unsigned int *radio_base = (volatile unsigned int *)map_base; 
	return (radio_base);
}

void radioTuner_tuneRadio(volatile unsigned int *ptrToRadio, float tune_frequency)
{
	float pinc = (-1.0*tune_frequency)*(float)(1<<27)/125.0e6;
	*(ptrToRadio+RADIO_TUNER_TUNER_PINC_OFFSET)=(int)pinc;

    // print updated freq and phase increment
    printf("\r\nRadio tuned to %d Hz\r\n", (int)tune_frequency);
    printf("Tuner DDS Frequency: %d Hz\r\n", (int)tune_frequency*-1);
    printf("Tuner DDS Phase Increment: %d\r\n", (int)pinc);
}

void radioTuner_setAdcFreq(volatile unsigned int* ptrToRadio, float freq)
{
	float pinc = freq*(float)(1<<27)/125.0e6;
	*(ptrToRadio+RADIO_TUNER_FAKE_ADC_PINC_OFFSET) = (int)pinc;

    // print updated freq and phase increment
    printf("\r\nFake ADC Frequency set to %d Hz\r\n", (int)freq);
    printf("Fake ADC DDS Phase Increment: %d\r\n", (int)pinc);
}

void play_tune(volatile unsigned int *ptrToRadio, float base_frequency)
{
	int i;
	float freqs[16] = {1760.0,1567.98,1396.91, 1318.51, 1174.66, 1318.51, 1396.91, 1567.98, 1760.0, 0, 1760.0, 0, 1760.0, 1975.53, 2093.0,0};
	float durations[16] = {1,1,1,1,1,1,1,1,.5,0.0001,.5,0.0001,1,1,2,0.0001};
	for (i=0;i<16;i++)
	{
		radioTuner_setAdcFreq(ptrToRadio,freqs[i]+base_frequency);
		usleep((int)(durations[i]*500000));
	}
}

void print_benchmark(volatile unsigned int *periph_base)
{
    // the below code does a little benchmark, reading from the peripheral a bunch 
    // of times, and seeing how many clocks it takes.  You can use this information
    // to get an idea of how fast you can generally read from an axi-lite slave device
    unsigned int start_time;
    unsigned int stop_time;
    start_time = *(periph_base+RADIO_TUNER_TIMER_REG_OFFSET);
    for (int i=0;i<2048;i++)
        stop_time = *(periph_base+RADIO_TUNER_TIMER_REG_OFFSET);
    printf("Elapsed time in clocks = %u\n",stop_time-start_time);
    // please insert your code here for calculate the actual throughput in Mbytes/second
    // how much data was transferred? How long did it take?
    unsigned int bytes_transferred = 2048*4; // each read of 32-bit reg (4 8-bit bytes)
    float time_spent = (stop_time-start_time)*8e-9; // 125MHz clock = 8 ns period
	float throughput = (bytes_transferred/1048576.0)/time_spent;
    printf("You transferred %u bytes of data in %f seconds\n",bytes_transferred,time_spent);
    printf("Measured Transfer throughput = %f Mbytes/sec\n",throughput);
}

// check if string is all numbers
bool check_numeric(char *str) 
{
    for(int i=0; i<strlen(str); i++) {
        if(!isdigit(str[i])) {
            return false;
        }
    }
    return true;
}

// separate thread for streaming data
void *streamThread(void *vargp) 
{
    while(1) {
        if (stream_on) {
            FIFO_count = *(my_fifo+SIMPLE_FIFO_COUNT_OFFSET);
            // if there are at least 256 words in the FIFO, read from FIFO
            if (FIFO_count >= 256) {
                // first 2 bytes of UDP frame are for the 16-bit counter
                udp_data[0] = frame_counter & 0xFF;
                udp_data[1] = frame_counter >> 8;

                // unload the FIFO
                for (int i=0; i<256; i++) {
                    word = *(my_fifo+SIMPLE_FIFO_DATA_OFFSET);

                    // LSB of signed I data
                    udp_data[4*i+2] = word & 0xFF;
                    // MSB of signed I data
                    udp_data[4*i+3] = (word >> 8) & 0xFF;
                    // LSB of signed Q data
                    udp_data[4*i+4] = (word >> 16) & 0xFF;
                    // MSB of signed Q data
                    udp_data[4*i+5] = (word >> 24) & 0xFF;
                }
            
                // send the frame
                if(sendto(sock, &udp_data, UDP_LEN, 0, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
                    fprintf(stderr, "Error sending packet\n");
                    exit(1);
                }
                else {
                    frame_counter++;
                }
            }
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr,"Invalid number of command line arguments. Please provide destination IP address.\r\n");
        exit(1);
    }

    // get a pointer to the peripheral base address using /dev/mem and the function mmap
    my_radio = get_a_pointer(RADIO_PERIPH_ADDRESS);	
    my_fifo = get_a_pointer(SIMPLE_FIFO_ADDRESS);

    // set up socket for UDP streaming
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Error opening socket\n");
        exit(1);
    }
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(argv[1]);
    servaddr.sin_port = htons(PORT);

    printf("\r\n\r\n\r\nLab 9 Sophia Seo - Final Linux SDR Lab\n\r");
    *(my_radio+RADIO_TUNER_CONTROL_REG_OFFSET) = 0; // make sure radio isn't in reset
    printf("\r\nEnter 't' to tune radio to a new frequency.\r\nEnter 'f' to set the fake ADC to a new frequency.\r\nEnter 'U/u' to increase fake ADC frequency by 1000/100 Hz.\r\nEnter 'D/d' to decrease fake ADC frequency by 1000/100 Hz.\r\nEnter 'r' to reset the DDS.\r\nEnter 's' to start/stop streaming data.\r\nEnter 'i' to set a new IP address.\r\nEnter [space] to repeat this menu.\r\n");

    // separate thread for streaming data
    pthread_t thread_id;
    pthread_create(&thread_id, NULL, streamThread, NULL);

    // main loop to react to user input
    while(1) {
    	char inChar = getchar();
    	switch(inChar) {
            // SET ADC FREQUENCY
            case 'f':
                // get frequency from user input
                printf("Enter new ADC frequency in Hz: ");
                while(getchar() != '\n') {
                    ;
                }
                fgets(freq_str, 10, stdin);
                freq_str[strcspn(freq_str, "\n")] = 0;    // remove newline
                // error/bounds checking on string
                if (check_numeric(freq_str)) {
                    printf("\r\nSetting frequency...\r\n");
                    adc_freq = atoi(freq_str);
                    if(adc_freq > 125000000) {
                        printf("Frequency can't be higher than 125000000. Setting to 125 MHz...\r\n");
                        adc_freq = 125000000;
                    }
                    // set ADC frequency
                    radioTuner_setAdcFreq(my_radio,adc_freq);
                }
                else {  // error if not a number
                    printf("\r\nInvalid input. Please enter numbers only. Frequency unchanged.\r\n");    
                }
                break;

            // SET TUNE FREQUENCY
            case 't':
                // get frequency from user input
                printf("Enter new tune frequency in Hz: ");
                while(getchar() != '\n') {
                    ;
                }
                fgets(freq_str, 10, stdin);
                freq_str[strcspn(freq_str, "\n")] = 0;    // remove newline

                // error/bounds checking on string
                if (check_numeric(freq_str)) {
                    printf("\r\nSetting frequency...\r\n");
                    tune_freq = atoi(freq_str);
                    if(tune_freq > 125000000) {
                        printf("Frequency can't be higher than 125000000. Setting to 125 MHz...\r\n");
                        tune_freq = 125000000;
                    }
                    // set tune frequency
                    radioTuner_tuneRadio(my_radio,tune_freq);
                }
                else {  // not a number
                    printf("\r\nInvalid input. Please enter numbers only. Frequency unchanged.\r\n");    
                }
                break;

            // INCREASE ADC FREQUENCY BY 1000 HZ
            case 'U':
                adc_freq = adc_freq + 1000;
                if (adc_freq > 125000000) {
                    printf("Frequency can't be higher than 125000000. Setting to 125 MHz...\r\n");
                    adc_freq = 125000000;
                }
                radioTuner_setAdcFreq(my_radio,adc_freq);
                break;

            // INCREASE ADC FREQUENCY BY 100 HZ
            case 'u':
                adc_freq = adc_freq + 100;
                if (adc_freq > 125000000) {
                    printf("Frequency can't be higher than 125000000. Setting to 125 MHz...\r\n");
                    adc_freq = 125000000;
                }
                radioTuner_setAdcFreq(my_radio,adc_freq);
                break;
        
            // DECREASE ADC FREQUENCY BY 1000 HZ
            case 'D':
                adc_freq = adc_freq - 1000;
                if (adc_freq < 0) {
                    printf("Frequency can't be lower than 0. Setting to 0 Hz...\r\n");
                    adc_freq = 0;
                }
                radioTuner_setAdcFreq(my_radio,adc_freq);
                break;

            // DECREASE ADC FREQUENCY BY 100 HZ
            case 'd':
                adc_freq = adc_freq - 100;
                if (adc_freq < 0) {
                    printf("Frequency can't be lower than 0. Setting to 0 Hz...\r\n");
                    adc_freq = 0;
                }
                radioTuner_setAdcFreq(my_radio,adc_freq);
                break;

            // RESET FAKE ADC DDS
            case 'r':
                *(my_radio+RADIO_TUNER_CONTROL_REG_OFFSET) = 1;
                *(my_radio+RADIO_TUNER_CONTROL_REG_OFFSET) = 0;
                break;

            // SET NEW DESTINATION IP ADDRESS
            case 'i':
                // get new IP from user input
                printf("Enter new destination IP address: ");
                fgets(dest_IP, 20, stdin);
                servaddr.sin_addr.s_addr = inet_addr(dest_IP);
                break;

            // TOGGLE UDP STREAMING ENABLED
            case 's':
                stream_on = !stream_on;
                if (stream_on) {
                    printf("Toggling UDP streaming ON\r\n");
                }
                else {
                    printf("Toggling UDP streaming OFF\r\n");
                }
                break;

            // REPRINT MENU
            case ' ':
                printf("\r\nEnter 't' to tune radio to a new frequency.\r\nEnter 'f' to set the fake ADC to a new frequency.\r\nEnter 'U/u' to increase fake ADC frequency by 1000/100 Hz.\r\nEnter 'D/d' to decrease fake ADC frequency by 1000/100 Hz.\r\nEnter 'r' to reset the DDS.\r\nEnter 's' to start/stop streaming data.\r\nEnter 'i' to set a new IP address.\r\nEnter [space] to repeat this menu.\r\n");
                break;

            case '\n':
                break;

            default:
                printf("\r\nKey input not recognized. Reprinting menu:\r\n");
                printf("\r\nEnter 't' to tune radio to a new frequency.\r\nEnter 'f' to set the fake ADC to a new frequency.\r\nEnter 'U/u' to increase fake ADC frequency by 1000/100 Hz.\r\nEnter 'D/d' to decrease fake ADC frequency by 1000/100 Hz.\r\nEnter 'r' to reset the DDS.\r\nEnter 's' to start/stop streaming data.\r\nEnter 'i' to set a new IP address.\r\nEnter [space] to repeat this menu.\r\n");
        }
    }
    return 0;
}
