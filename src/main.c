/*
* This file is part of the hoverboard-firmware-hack project.
*
* Copyright (C) 2017-2018 Rene Hopf <renehopf@mac.com>
* Copyright (C) 2017-2018 Nico Stute <crinq@crinq.de>
* Copyright (C) 2017-2018 Niklas Fauth <niklas.fauth@kit.fail>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stm32f1xx_hal.h"
#include "defines.h"
#include "setup.h"
#include "config.h"
#include "comms.h"
#include "sensorcoms.h"
#include "flashaccess.h"
#include "protocolfunctions.h"
#include "bldc.h"
#include "hallinterrupts.h"
#include "softwareserial.h"
//#include "hd44780.h"
#include "pid.h"
#include "flashcontent.h"

#include "deadreckoner.h"
#include "control_structures.h"

#include <string.h>
void BldcController_Init();

void SystemClock_Config(void);

extern TIM_HandleTypeDef htim_left;
extern TIM_HandleTypeDef htim_right;
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;
extern volatile adc_buf_t adc_buffer;
extern I2C_HandleTypeDef hi2c2;
extern UART_HandleTypeDef huart2;

extern volatile long long bldc_counter;
int cmd1, cmd1_ADC, adcrFiltered;  // normalized input values. -1000 to 1000
int cmd2, cmd2_ADC, adctFiltered;

// used if set in setup.c
int autoSensorBaud2 = 0; // in USART2_IT_init
int autoSensorBaud3 = 0; // in USART3_IT_init

bool ADCcontrolActive = false;

int sensor_control = 0;



// the main thing qwhich determines how we are controlled from protocol
int control_type = CONTROL_TYPE_NONE;

#ifdef READ_SENSOR
SENSOR_DATA last_sensor_data[2];
int sensor_stabilise = 0;

#endif
uint8_t disablepoweroff = 0;
int powerofftimer = 0;


extern volatile unsigned int timerval;
extern volatile unsigned int ssbits;

uint8_t button1, button2, button1_ADC, button2_ADC;

int steer; // global variable for steering. -1000 to 1000
int speed; // global variable for speed. -1000 to 1000

extern volatile int pwml;  // global variable for pwm left. -1000 to 1000
extern volatile int pwmr;  // global variable for pwm right. -1000 to 1000

extern uint8_t buzzerFreq;    // global variable for the buzzer pitch. can be 1, 2, 3, 4, 5, 6, 7...
extern uint8_t buzzerPattern; // global variable for the buzzer pattern. can be 1, 2, 3, 4, 5, 6, 7...
int buzzerLen = 0;

extern uint8_t enable; // global variable for motor enable

extern volatile uint32_t input_timeout_counter; // global variable for input timeout
extern float batteryVoltage; // global variable for battery voltage

uint32_t inactivity_timeout_counter;
uint32_t debug_counter = 0;

extern uint8_t nunchuck_data[6];
#ifdef CONTROL_PPM
extern volatile uint16_t ppm_captured_value[PPM_NUM_CHANNELS+1];
#endif

int milli_vel_error_sum = 0;

DEADRECKONER *deadreconer;
INTEGER_XYT_POSN xytPosn;

typedef struct tag_power_button_info {
  int startup_button_held;      // indicates power button was active at startup
  int button_prev;              // last value of power button
  unsigned int button_held_ms;  // ms for which the button has been held down
} POWER_BUTTON_INFO;
void check_power_button();

POWER_BUTTON_INFO power_button_info;

void poweroff() {
    if (ABS(speed) < 20) {
        buzzerPattern = 0;
        enable = 0;
        for (int i = 0; i < 8; i++) {
            buzzerFreq = i;
            HAL_Delay(100);
        }
        HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, 0);

        // if we are powered from sTLink, this bit allows the system to be started again with the button.
        while (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {}

        while (1){
          if (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)){
            HAL_NVIC_SystemReset();
          }
        }
    }
}

// actually 'power'
int pwms[2] = {0, 0};

// unused, but keep ascii from erroring
int dspeeds[2] = {0,0};



/////////////////////////////////////////
// variables stored in flash
// from flashcontent.h
FLASH_CONTENT FlashContent;
const FLASH_CONTENT FlashDefaults = FLASH_DEFAULTS;


typedef struct tag_PID_FLOATS{
    float in;
    float set;
    float out;

    int count; // - used in averaging speed between pid loops
} PID_FLOATS;

// setup pid control for left and right speed.
pid_controller  PositionPid[2];
// temp floats
PID_FLOATS PositionPidFloats[2] = {
  { 0, 0, 0,   0 },
  { 0, 0, 0,   0 }
};
pid_controller  SpeedPid[2];
// temp floats
PID_FLOATS SpeedPidFloats[2] = {
  { 0, 0, 0,   0 },
  { 0, 0, 0,   0 }
};

void init_PID_control(){
  memset(&PositionPid, 0, sizeof(PositionPid));
  memset(&SpeedPid, 0, sizeof(SpeedPid));
  for (int i = 0; i < 2; i++){
    PositionPidFloats[i].in = 0;
    PositionPidFloats[i].set = 0;
    pid_create(&PositionPid[i], &PositionPidFloats[i].in, &PositionPidFloats[i].out, &PositionPidFloats[i].set,
      (float)FlashContent.PositionKpx100/100.0,
      (float)FlashContent.PositionKix100/100.0,
      (float)FlashContent.PositionKdx100/100.0);

    // maximum pwm outputs for positional control; limits speed
  	pid_limits(&PositionPid[i], -FlashContent.PositionPWMLimit, FlashContent.PositionPWMLimit);
  	pid_auto(&PositionPid[i]);
    SpeedPidFloats[i].in = 0;
    SpeedPidFloats[i].set = 0;
    pid_create(&SpeedPid[i], &SpeedPidFloats[i].in, &SpeedPidFloats[i].out, &SpeedPidFloats[i].set,
      (float)FlashContent.SpeedKpx100/100.0,
      (float)FlashContent.SpeedKix100/100.0,
      (float)FlashContent.SpeedKdx100/100.0);

    // maximum increment to pwm outputs for speed control; limits changes in speed (accelleration)
  	pid_limits(&SpeedPid[i], -FlashContent.SpeedPWMIncrementLimit, FlashContent.SpeedPWMIncrementLimit);
  	pid_auto(&SpeedPid[i]);
  }
}

void change_PID_constants(){
  for (int i = 0; i < 2; i++){
    pid_tune(&PositionPid[i],
      (float)FlashContent.PositionKpx100/100.0,
      (float)FlashContent.PositionKix100/100.0,
      (float)FlashContent.PositionKdx100/100.0);
  	pid_limits(&PositionPid[i], -FlashContent.PositionPWMLimit, FlashContent.PositionPWMLimit);

    pid_tune(&SpeedPid[i],
      (float)FlashContent.SpeedKpx100/100.0,
      (float)FlashContent.SpeedKix100/100.0,
      (float)FlashContent.SpeedKdx100/100.0);
  	pid_limits(&SpeedPid[i], -FlashContent.SpeedPWMIncrementLimit, FlashContent.SpeedPWMIncrementLimit);
  }
}

#ifdef FLASH_STORAGE
void init_flash_content(){
  FLASH_CONTENT FlashRead;
  int len = readFlash( (unsigned char *)&FlashRead, sizeof(FlashRead) );

  if ((len != sizeof(FlashRead)) || (FlashRead.magic != CURRENT_MAGIC)){
    memcpy(&FlashRead, &FlashDefaults, sizeof(FlashRead));
    writeFlash( (unsigned char *)&FlashRead, sizeof(FlashRead) );
    consoleLog("Flash initiailised\r\n");
  }
  memcpy(&FlashContent, &FlashRead, sizeof(FlashContent));
}
#endif


int main(void) {
  HAL_Init();
  __HAL_RCC_AFIO_CLK_ENABLE();
  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
  /* System interrupt init*/
  /* MemoryManagement_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(MemoryManagement_IRQn, 0, 0);
  /* BusFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(BusFault_IRQn, 0, 0);
  /* UsageFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(UsageFault_IRQn, 0, 0);
  /* SVCall_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SVCall_IRQn, 0, 0);
  /* DebugMonitor_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DebugMonitor_IRQn, 0, 0);
  /* PendSV_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(PendSV_IRQn, 0, 0);
  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 1, 0);

#define WHEELBASE_MM 525.0
#ifdef HALL_INTERRUPTS
  deadreconer = DeadReckoner(
    & HallData[0].HallPosn,
    & HallData[1].HallPosn,
    HALL_POSN_PER_REV,
    (DEFAULT_WHEEL_SIZE_INCHES*25.4),
    WHEELBASE_MM, 1);
#endif

  SystemClock_Config();

  __HAL_RCC_DMA1_CLK_DISABLE();
  MX_GPIO_Init();
  MX_TIM_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();

  #if defined(DEBUG_SERIAL_USART2) || defined(DEBUG_SERIAL_USART3)
    UART_Init();
  #endif

  memset((void*)&electrical_measurements, 0, sizeof(electrical_measurements));

  HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, 1);

  HAL_ADC_Start(&hadc1);
  HAL_ADC_Start(&hadc2);

  #ifdef READ_SENSOR
  if (USART2_BAUD_SENSE) {
    autoSensorBaud2 = getSensorBaudRate(0);
  }
  if (USART3_BAUD_SENSE) {
    autoSensorBaud3 = getSensorBaudRate(1);
  }
  #endif

  #ifdef SERIAL_USART2_IT
  USART2_IT_init();
  #endif
  #ifdef SERIAL_USART3_IT
  USART3_IT_init();
  #endif
  #ifdef SOFTWARE_SERIAL
  SoftwareSerialInit();
  #endif

#ifdef FLASH_STORAGE
  init_flash_content();

  init_PID_control();
#endif

  #ifdef READ_SENSOR
  // initialise to 9 bit interrupt driven comms on USART 2 & 3
  sensor_init();
  #endif

  if (0 == FlashContent.MaxCurrLim) {
    FlashContent.MaxCurrLim = DC_CUR_LIMIT*100;
  }
  electrical_measurements.dcCurLim = MIN(DC_CUR_LIMIT*100, FlashContent.MaxCurrLim);



  for (int i = 8; i >= 0; i--) {
    buzzerFreq = i;
    HAL_Delay(100);
  }
  buzzerFreq = 0;

  HAL_GPIO_WritePin(LED_PORT, LED_PIN, 1);

  //int lastspeeds[2] = {0, 0};

  #ifdef READ_SENSOR
  // things we use in main loop for sensor control
  consoleLog("power on\n");

  #endif
  #ifdef HALL_INTERRUPTS
    // enables interrupt reading of hall sensors for dead reconing wheel position.
    HallInterruptinit();
  #endif

  #ifdef CONTROL_PPM
    PPM_Init();
  #endif

  #ifdef CONTROL_NUNCHUCK
    I2C_Init();
    Nunchuck_Init();
  #endif



  // sets up serial ports, and enables protocol on selected ports
  setup_protocol();

  int last_control_type = CONTROL_TYPE_NONE;


  float board_temp_adc_filtered = (float)adc_buffer.temp;
  float board_temp_deg_c;

  enable = 1;  // enable motors

  // ####### POWER-BUTTON startup conditions #######
  power_button_info.startup_button_held = HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN);
  power_button_info.button_prev = power_button_info.startup_button_held;
  power_button_info.button_held_ms = 0;

  if (power_button_info.startup_button_held) {
    consoleLog("Power button down at startup\r\n");
  } else {
    consoleLog("Power button up at startup\r\n");
  }


  timeStats.hclkFreq = HAL_RCC_GetHCLKFreq();

  timeStats.now_us = HallGetuS();
  timeStats.now_ms = HAL_GetTick();
  timeStats.nominal_delay_us = (DELAY_IN_MAIN_LOOP * 1000);
  timeStats.start_processing_us = timeStats.now_us + timeStats.nominal_delay_us;
  timeStats.start_processing_ms = timeStats.now_ms + DELAY_IN_MAIN_LOOP;

  long long start_bldc_counter = bldc_counter;
  HAL_Delay(200);
  long long bldc_counter_200ms = bldc_counter;

  int bldc_in_200ms = (int)(bldc_counter_200ms - start_bldc_counter);
  timeStats.bldc_freq = bldc_in_200ms * 5;

  // uses timeStats.bldc_freq
  BldcControllerParams.callFrequency = timeStats.bldc_freq;
  BldcController_Init();

  while(1) {
    timeStats.time_in_us = timeStats.now_us;
    timeStats.time_in_ms = timeStats.now_ms;

    if (timeStats.start_processing_us < timeStats.now_us) {
      timeStats.us_lost += timeStats.now_us - timeStats.start_processing_us;
      timeStats.main_late_count++;
      timeStats.start_processing_us = timeStats.now_us + 1000;  // at least 1ms of delay
    }

    // delay until we should start processing
    while (timeStats.now_us < timeStats.start_processing_us){
      #if (INCLUDE_PROTOCOL == INCLUDE_PROTOCOL2)
        #ifdef SOFTWARE_SERIAL
          while ( softwareserial_available() > 0 ) {
            protocol_byte( &sSoftwareSerial, (unsigned char) softwareserial_getrx() );
          }
          protocol_tick( &sSoftwareSerial );
        #endif

        #if defined(SERIAL_USART2_IT) && defined(READ_SENSOR)
          // if we enabled USART2 as protocol from power button at startup
          if (USART2ProtocolEnable) {
            while ( serial_usart_buffer_count(&usart2_it_RXbuffer) > 0 ) {
              protocol_byte( &sUSART2, (unsigned char) serial_usart_buffer_pop(&usart2_it_RXbuffer) );
            }
            protocol_tick( &sUSART2 );
          }
        #endif

        #if defined(SERIAL_USART2_IT) && !defined(READ_SENSOR)
          while ( serial_usart_buffer_count(&usart2_it_RXbuffer) > 0 ) {
            protocol_byte( &sUSART2, (unsigned char) serial_usart_buffer_pop(&usart2_it_RXbuffer) );
          }
          protocol_tick( &sUSART2 );
        #endif

        #if defined(SERIAL_USART3_IT) && !defined(READ_SENSOR)
          while ( serial_usart_buffer_count(&usart3_it_RXbuffer) > 0 ) {
            protocol_byte( &sUSART3, (unsigned char) serial_usart_buffer_pop(&usart3_it_RXbuffer) );
          }
          protocol_tick( &sUSART3 );
        #endif
      #endif
      timeStats.now_us = HallGetuS();
      timeStats.now_ms = HAL_GetTick();
    }

    // move out '5ms' trigger on by 5ms
    timeStats.processing_in_us = timeStats.now_us;
    timeStats.processing_in_ms = timeStats.now_ms;

    timeStats.bldc_us = (1000*timeStats.bldc_cycles)/(timeStats.hclkFreq/1000);

    // read last DMAed ADC values, moved from bldc interrupt to non interrupt.
    readADCs();

    /////////////////////////////////////
    // proceesing starts after we hit 5ms interval
    computePosition(deadreconer);
    double x,y,t;
    getXYT(deadreconer, &x, &y, &t);

    xytPosn.x = (int)x;
    xytPosn.y = (int)y;
    xytPosn.degrees = (int)t;

    cmd1 = 0;
    cmd2 = 0;

    #ifdef CONTROL_NUNCHUCK
      Nunchuck_Read();
      cmd1 = CLAMP((nunchuck_data[0] - 127) * 8, -1000, 1000); // x - axis. Nunchuck joystick readings range 30 - 230
      cmd2 = CLAMP((nunchuck_data[1] - 128) * 8, -1000, 1000); // y - axis

      button1 = (uint8_t)nunchuck_data[5] & 1;
      button2 = (uint8_t)(nunchuck_data[5] >> 1) & 1;
    #endif

    #ifdef CONTROL_PPM
      cmd1 = CLAMP((ppm_captured_value[0] - 500) * 2, -1000, 1000);
      cmd2 = CLAMP((ppm_captured_value[1] - 500) * 2, -1000, 1000);
      button1 = ppm_captured_value[5] > 500;
      float scale = ppm_captured_value[2] / 1000.0f;
    #endif

#ifdef CONTROL_ADC
      // ADC values range: 0-4095, see ADC-calibration in config.h

      adcrFiltered = adcrFiltered * (1.0 - ADC_OFF_FILTER) + adc_buffer.l_rx2 * ADC_OFF_FILTER;
      adctFiltered = adctFiltered * (1.0 - ADC_OFF_FILTER) + adc_buffer.l_tx2 * ADC_OFF_FILTER;



      if(adc_buffer.l_tx2 < ADC1_ZERO) {
        cmd1_ADC = (CLAMP(adc_buffer.l_tx2, ADC1_MIN, ADC1_ZERO) - ADC1_ZERO) / ((ADC1_ZERO - ADC1_MIN) / ADC1_MULT_NEG); // ADC1 - Steer
      } else {
        cmd1_ADC = (CLAMP(adc_buffer.l_tx2, ADC1_ZERO, ADC1_MAX) - ADC1_ZERO) / ((ADC1_MAX - ADC1_ZERO) / ADC1_MULT_POS); // ADC1 - Steer
      }

      if(adc_buffer.l_rx2 < ADC2_ZERO) {
        cmd2_ADC = (CLAMP(adc_buffer.l_rx2, ADC2_MIN, ADC2_ZERO) - ADC2_ZERO) / ((ADC2_ZERO - ADC2_MIN) / ADC2_MULT_NEG); // ADC2 - Speed
      } else {
        cmd2_ADC = (CLAMP(adc_buffer.l_rx2, ADC2_ZERO, ADC2_MAX) - ADC2_ZERO) / ((ADC2_MAX - ADC2_ZERO) / ADC2_MULT_POS); // ADC2 - Speed
      }

      if(ADC_SWITCH_CHANNELS) {
        int cmdTemp = cmd1_ADC;
        cmd1_ADC = cmd2_ADC;
        cmd2_ADC = cmdTemp;
        if((adctFiltered < ADC_OFF_START) || (adctFiltered > ADC_OFF_END) ) {
          ADCcontrolActive = true;
        } else {
          if(ADCcontrolActive) {
            cmd1 = 0;
            cmd2 = 0;
          }
          ADCcontrolActive = false;
        }
      } else {
        if((adcrFiltered < ADC_OFF_START) || (adcrFiltered > ADC_OFF_END) ) {
          ADCcontrolActive = true;
        } else {
          if(ADCcontrolActive) {
            cmd1 = 0;
            cmd2 = 0;
          }
          ADCcontrolActive = false;
        }
      }

      if(ADC_REVERSE_STEER) {
        cmd1_ADC = -cmd1_ADC;
      }
      // use ADCs as button inputs:
      button1_ADC = (uint8_t)(adc_buffer.l_tx2 > 2000);  // ADC1
      button2_ADC = (uint8_t)(adc_buffer.l_rx2 > 2000);  // ADC2
#endif

    if (!power_button_info.startup_button_held){
    #ifdef READ_SENSOR
      // read the last sensor message in the buffer
      sensor_read_data();

      // tapp one or other side twice in 2s, with at least 1/4s between to
      // enable hoverboard mode.
      if (CONTROL_TYPE_NONE == control_type){
        if (sensor_data[0].doubletap || sensor_data[1].doubletap){
          if (FlashContent.HoverboardEnable){
            sensor_control = 1;
          }
          consoleLog("double tap -> hoverboard mode\r\n");
          sensor_data[0].doubletap = 0;
          sensor_data[1].doubletap = 0;
        }
      }

      if (electrical_measurements.charging){
        sensor_set_flash(0, 3);
      } else {
        sensor_set_flash(0, 0);
      }

      // if roll is a large angle (>20 degrees)
      // then disable
      #ifdef CONTROL_SENSOR
        int setcolours = 1;
        if (sensor_control && FlashContent.HoverboardEnable){
          if ((!sensor_data[0].rollhigh) || (!sensor_data[1].rollhigh)){
            if (enable) {
              consoleLog("disable by rollHigh\r\n");
            }
            enable = 0;
          } else {
            if(!electrical_measurements.charging){
              int either_sensor_ok = sensor_data[0].sensor_ok || sensor_data[1].sensor_ok;
              int dirs[2] = {-1, 1};

              for (int i = 0; i < 2; i++){
                if (sensor_data[i].sensor_ok){
                  pwms[i] = CLAMP(dirs[i]*(sensor_data[i].complete.Angle - sensor_data[i].Center)/3, -FlashContent.HoverboardPWMLimit, FlashContent.HoverboardPWMLimit);
                  sensor_set_colour(i, SENSOR_COLOUR_YELLOW);
                  if (!enable) {
                    consoleLog("enable by hoverboard mode & !rollHigh\r\n");
                    enable = 1;
                  }
                } else {
                  pwms[i] = 0;
                  if (enable && !either_sensor_ok) {
                    consoleLog("disable by hoverboard mode & step off\r\n");
                    enable = 0;
                  }
                  sensor_set_colour(i, SENSOR_COLOUR_GREEN);
                }
              }
              // don't set default cilours below
              setcolours = 0;
              input_timeout_counter = 0;
              inactivity_timeout_counter = 0;
            } else {
              pwms[0] = pwms[1] = 0;
              input_timeout_counter = 0;
              if (enable) {
                consoleLog("disable by charging\r\n");
                enable = 0;
              }
            }
          }
        }

        // if not set above, set default colours now
        // changed so that  it did not cycle btween green/yellow
        if(setcolours) {
          for (int i = 0; i < 2; i++){
            if  (sensor_data[i].sensor_ok){
              sensor_set_colour(i, SENSOR_COLOUR_GREEN);
            } else {
              sensor_set_colour(i, SENSOR_COLOUR_RED);
            }
          }
        }
      #endif // end if control_sensor

    #endif // READ_SENSOR

    #if defined(INCLUDE_PROTOCOL)||defined(READ_SENSOR)
        if (!sensor_control || !FlashContent.HoverboardEnable){
          if ((last_control_type != control_type) || (!enable)){
            // nasty things happen if it's not re-initialised
            init_PID_control();
            last_control_type = control_type;
          }

          switch (control_type){
            case CONTROL_TYPE_POSITION:
              for (int i = 0; i < 2; i++){
                if (pid_need_compute(&PositionPid[i])) {
                  // Read process feedback
#ifdef HALL_INTERRUPTS
                  PositionPidFloats[i].set = PosnData.wanted_posn_mm[i];
                  PositionPidFloats[i].in = HallData[i].HallPosn_mm;
#endif
                  // Compute new PID output value
                  pid_compute(&PositionPid[i]);
                  //Change actuator value
                  int pwm = PositionPidFloats[i].out;
                  pwms[i] = pwm;
                  #ifdef LOG_PWM
                    if (i == 0){
                      sprintf(tmp, "%d:%d\r\n", i, pwm);
                      consoleLog(tmp);
                    }
                  #endif
                }
              }
              break;
            case CONTROL_TYPE_SPEED:
              for (int i = 0; i < 2; i++){
                // average speed over all the loops until pid_need_compute() returns !=0
#ifdef HALL_INTERRUPTS
                SpeedPidFloats[i].in += HallData[i].HallSpeed_mm_per_s;
#endif
                SpeedPidFloats[i].count++;
                if (!enable){ // don't want anything building up
                  SpeedPidFloats[i].in = 0;
                  SpeedPidFloats[i].count = 1;
                }
                if (pid_need_compute(&SpeedPid[i])) {
                  // Read process feedback
                  int belowmin = 0;
                  // won't work below about 45
                  if (ABS(SpeedData.wanted_speed_mm_per_sec[i]) < SpeedData.speed_minimum_speed){
                    SpeedPidFloats[i].set = 0;
                    belowmin = 1;
                  } else {
                    SpeedPidFloats[i].set = SpeedData.wanted_speed_mm_per_sec[i];
                  }
                  SpeedPidFloats[i].in = SpeedPidFloats[i].in/(float)SpeedPidFloats[i].count;
                  SpeedPidFloats[i].count = 0;
                  // Compute new PID output value
                  pid_compute(&SpeedPid[i]);
                  //Change actuator value
                  int pwm = SpeedPidFloats[i].out;
                  if (belowmin){
                    pwms[i] = 0;
                  } else {
                    pwms[i] =
                      CLAMP(pwms[i] + pwm, -SpeedData.speed_max_power, SpeedData.speed_max_power);
                  }
                  #ifdef LOG_PWM
                  if (i == 0){
                    sprintf(tmp, "%d:%d(%d) S:%d H:%d\r\n", i, pwms[i], pwm, (int)SpeedPidFloats[i].set, (int)SpeedPidFloats[i].in);
                    consoleLog(tmp);
                  }
                  #endif
                }
              }
              break;

            case CONTROL_TYPE_PWM:
              for (int i = 0; i < 2; i++){
                pwms[i] = PWMData.pwm[i];
              }
              break;
          }
        }
      }

      #if defined CONTROL_ADC
        if(ADCcontrolActive) {
          cmd1 = cmd1_ADC;
          cmd2 = cmd2_ADC;
          input_timeout_counter = 0;
        }
      #endif

      #ifdef READ_SENSOR
        // send twice to make sure each side gets it.
        // if we sent diagnositc data, it seems to need this.
        sensor_send_lights();
      #endif

    #endif // INCLUDE_PROTOCOL)||defined(READ_SENSOR)

      // ####### LOW-PASS FILTER #######
      steer = steer * (1.0 - FILTER) + cmd1 * FILTER;
      speed = speed * (1.0 - FILTER) + cmd2 * FILTER;


      // ####### MIXER #######
    #ifdef INCLUDE_PROTOCOL
      if(ADCcontrolActive) {
    #else
      if(1) {
    #endif

        if(ADC_TANKMODE && ADCcontrolActive) {
          pwms[0] = pwms[0] * (1.0 - FILTER) + cmd1 * FILTER;
          pwms[1] = pwms[1] * (1.0 - FILTER) + cmd2 * FILTER;
        } else {
          pwms[0] = CLAMP(speed * SPEED_COEFFICIENT -  steer * STEER_COEFFICIENT, -1000, 1000);
          pwms[1] = CLAMP(speed * SPEED_COEFFICIENT +  steer * STEER_COEFFICIENT, -1000, 1000);
        }
      }

    if(SWITCH_WHEELS) {
      int tmppwm = pwms[1];
      pwms[1] = pwms[0];
      pwms[0] = tmppwm;
    }

    #ifdef ADDITIONAL_CODE
      ADDITIONAL_CODE;
    #endif
      if (!enable){
        pwms[0] = pwms[1] = 0;
      }

    if(INVERT_R_DIRECTION) {
      pwmr = pwms[1];
    } else {
      pwmr = -pwms[1];
    }

    if(INVERT_L_DIRECTION) {
      pwml = -pwms[0];
    } else {
      pwml = pwms[0];
    }


//    for (int i = 0; i < 2; i++){
//      lastspeeds[i] = pwms[i];
//    }

    if ((debug_counter++) % 100 == 0) {
      // ####### CALC BOARD TEMPERATURE #######
	  if (board_temp_adc_filtered == 0) board_temp_adc_filtered = (float)adc_buffer.temp;
      else board_temp_adc_filtered = board_temp_adc_filtered * 0.99 + (float)adc_buffer.temp * 0.01;
      board_temp_deg_c = ((float)TEMP_CAL_HIGH_DEG_C - (float)TEMP_CAL_LOW_DEG_C) / ((float)TEMP_CAL_HIGH_ADC - (float)TEMP_CAL_LOW_ADC) * (board_temp_adc_filtered - (float)TEMP_CAL_LOW_ADC) + (float)TEMP_CAL_LOW_DEG_C;

      electrical_measurements.board_temp_raw = adc_buffer.temp;
      electrical_measurements.board_temp_filtered = board_temp_adc_filtered;
      electrical_measurements.board_temp_deg_c = board_temp_deg_c;
      electrical_measurements.charging = !(CHARGER_PORT->IDR & CHARGER_PIN);

      // ####### DEBUG SERIAL OUT #######
      #ifdef CONTROL_ADC
        setScopeChannel(0, (int)adc_buffer.l_tx2);  // 1: ADC1
        setScopeChannel(1, (int)adc_buffer.l_rx2);  // 2: ADC2
      #endif

      #ifdef CONTROL_SENSOR
        setScopeChannel(0, (int)sensor_data[0].complete.Angle);  // 1: ADC1
        setScopeChannel(1, -(int)sensor_data[1].complete.Angle);  // 2: ADC2
      #endif

      setScopeChannel(2, (int)pwms[1]);  // 3: output speed: 0-1000
      setScopeChannel(3, (int)pwms[0]);  // 4: output speed: 0-1000
      setScopeChannel(4, (int)adc_buffer.batt1);  // 5: for battery voltage calibration
      setScopeChannel(5, (int)(batteryVoltage * 100.0f));  // 6: for verifying battery voltage calibration
      setScopeChannel(6, (int)board_temp_adc_filtered);  // 7: for board temperature calibration
      setScopeChannel(7, (int)board_temp_deg_c);  // 8: for verifying board temperature calibration
      consoleScope();

//      SoftwareSerialReadTimer();
    }


    // does things if power button is pressed, held.
    check_power_button();


    // if we plug in the charger, keep us alive
    // also if we have deliberately turned off poweroff over serial
    if (electrical_measurements.charging || disablepoweroff){
      inactivity_timeout_counter = 0;
    }

    // ####### BEEP AND EMERGENCY POWEROFF #######
    if (TEMP_POWEROFF_ENABLE && board_temp_deg_c >= TEMP_POWEROFF && ABS(speed) < 20){  // poweroff before mainboard burns OR low bat 3
      consoleLog("power off by temp\r\n");
      poweroff();
    }

    if (batteryVoltage < ((float)BAT_LOW_DEAD * (float)BAT_NUMBER_OF_CELLS) && ABS(speed) < 20) {  // poweroff before mainboard burns OR low bat 3
      consoleLog("power off by low voltage\r\n");
      poweroff();
    } else if (TEMP_WARNING_ENABLE && board_temp_deg_c >= TEMP_WARNING) {  // beep if mainboard gets hot
      buzzerFreq = 4;
      buzzerPattern = 1;
    } else if (batteryVoltage < ((float)BAT_LOW_LVL1 * (float)BAT_NUMBER_OF_CELLS) && batteryVoltage > ((float)BAT_LOW_LVL2 * (float)BAT_NUMBER_OF_CELLS) && BAT_LOW_LVL1_ENABLE) {  // low bat 1: slow beep
      buzzerFreq = 5;
      buzzerPattern = 42;
    } else if (batteryVoltage < ((float)BAT_LOW_LVL2 * (float)BAT_NUMBER_OF_CELLS) && batteryVoltage > ((float)BAT_LOW_DEAD * (float)BAT_NUMBER_OF_CELLS) && BAT_LOW_LVL2_ENABLE) {  // low bat 2: fast beep
      buzzerFreq = 5;
      buzzerPattern = 6;
    } else if (BEEPS_BACKWARD && speed < -50) {  // backward beep
      buzzerFreq = 5;
      buzzerPattern = 1;
    } else {  // do not beep
      if (buzzerLen > 0){
        buzzerLen--;
      } else {
        buzzerFreq = 0;
        buzzerPattern = 0;
      }
    }


    // ####### INACTIVITY TIMEOUT #######
    if (ABS(pwms[0]) > 50 || ABS(pwms[1]) > 50) {
      inactivity_timeout_counter = 0;
    } else {
      inactivity_timeout_counter ++;
    }

    // inactivity 10s warning; 1s bleeping
    if ((inactivity_timeout_counter > (INACTIVITY_TIMEOUT * 50 * 1000) / DELAY_IN_MAIN_LOOP) &&
        (buzzerFreq == 0)) {
      buzzerFreq = 3;
      buzzerPattern = 1;
      buzzerLen = 1000;
    }

    // inactivity 5s warning; 1s bleeping
    if ((inactivity_timeout_counter > (INACTIVITY_TIMEOUT * 55 * 1000) / DELAY_IN_MAIN_LOOP) &&
        (buzzerFreq == 0)) {
      buzzerFreq = 2;
      buzzerPattern = 1;
      buzzerLen = 1000;
    }

    // power off after ~60s of inactivity
    if (inactivity_timeout_counter > (INACTIVITY_TIMEOUT * 60 * 1000) / DELAY_IN_MAIN_LOOP ) {  // rest of main loop needs maybe 1ms
      inactivity_timeout_counter = 0;
      consoleLog("power off by 60s inactivity\r\n");
      poweroff();
    }


    if (powerofftimer > 0){
      powerofftimer --;

      // spit a msg every 2 seconds
      if (!(powerofftimer % (2000/DELAY_IN_MAIN_LOOP))){
        char tmp[30];
        sprintf(tmp, "power off in %ds\r\n", (powerofftimer*DELAY_IN_MAIN_LOOP)/1000 );
        consoleLog(tmp);
      }

      if (powerofftimer <= 10000/DELAY_IN_MAIN_LOOP){
        buzzerFreq = 3;
        buzzerPattern = 1;
        buzzerLen = 1000;
      }

      if (powerofftimer <= 5000/DELAY_IN_MAIN_LOOP){
        buzzerFreq = 2;
        buzzerPattern = 1;
        buzzerLen = 1000;
      }

      if (powerofftimer <= 0){
        powerofftimer = 0;
        consoleLog("power off by timer\r\n");
        poweroff();
      }
    }
    ////////////////////////////////
    // take stats
    timeStats.now_us = HallGetuS();
    timeStats.now_ms = HAL_GetTick();

    timeStats.main_interval_us = timeStats.now_us - timeStats.time_in_us;
    timeStats.main_interval_ms = timeStats.now_ms - timeStats.time_in_ms;
    timeStats.main_delay_us = timeStats.processing_in_us - timeStats.time_in_us;
    timeStats.main_delay_ms = timeStats.processing_in_ms - timeStats.time_in_ms;
    timeStats.main_processing_us = timeStats.now_us - timeStats.processing_in_us;
    timeStats.main_processing_ms = timeStats.now_ms - timeStats.processing_in_ms;

    // maybe average main_dur as a stat?
    if (timeStats.main_interval_ms == 0){
      timeStats.main_interval_ms = ((float)timeStats.main_interval_us)/1000;
      timeStats.main_processing_ms = ((float)timeStats.main_processing_us)/1000.0;
    }
    timeStats.f_main_interval_ms = timeStats.f_main_interval_ms * 0.99;
    timeStats.f_main_interval_ms = timeStats.f_main_interval_ms + (((float)timeStats.main_interval_us)/1000.0)*0.01;

    timeStats.f_main_processing_ms = timeStats.f_main_processing_ms * 0.99;
    timeStats.f_main_processing_ms = timeStats.f_main_processing_ms + (((float)timeStats.main_processing_us)/1000.0)*0.01;

    // select next loop start point
    // move out '5ms' trigger on by 5ms
    timeStats.start_processing_us = timeStats.start_processing_us + timeStats.nominal_delay_us;

    ////////////////////////////////
    // increase input_timeout_counter
    if(!enable_immediate) input_timeout_counter++;
  }
}


//////////////////////////////////////////////////////
// check and do things when we press the power button
void check_power_button(){

  // if power button is currently down
  if (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {
    // increment how long it has been down for.
    power_button_info.button_held_ms += DELAY_IN_MAIN_LOOP;

    // if power button was down at start
    if (power_button_info.startup_button_held) {
      if (power_button_info.button_held_ms > 5000)
      {
        // provisional startup mode - do something if button held for 5s at startup
      }
    } else {

      // if we have seen the state released since startup
      if (power_button_info.button_prev == 0){
        power_button_info.button_prev = 1;
        // reset the time pressed to zero
        power_button_info.button_held_ms = 0;
      } else {
        // button remains pressed
        // indicate how long it has been pressed by colour
        if ((power_button_info.button_held_ms > 100) &&
            (power_button_info.button_held_ms < 2000) )
        {
        #if defined CONTROL_SENSOR
          // indicate with 1 flash that this would power off
          sensor_set_flash(0, 1);
          sensor_set_flash(1, 1);
        #endif
        }
        if ((power_button_info.button_held_ms >= 2000) &&
            (power_button_info.button_held_ms < 5000) )
        {
        #if defined CONTROL_SENSOR
          // indicate with 2 flashes that we would not power off, and not calibrate
          sensor_set_flash(0, 2);
          sensor_set_flash(1, 2);
        #endif
        }
        if ((power_button_info.button_held_ms > 5000) &&
            (power_button_info.button_held_ms < 10000) )
        {
        #if defined CONTROL_SENSOR
          // indicate with 3 flashes that we would calibrate
          sensor_set_flash(0, 3);
          sensor_set_flash(1, 3);
        #endif
        }
        if ((power_button_info.button_held_ms > 10000) &&
            (power_button_info.button_held_ms < 15000) )
        {
        #if defined CONTROL_SENSOR
          // indicate with 4 flashes that we would turn USART2 into control
          sensor_set_flash(0, 4);
          sensor_set_flash(1, 4);
        #endif
        }
        if ((power_button_info.button_held_ms > 15000) &&
            (power_button_info.button_held_ms < 100000) )
        {
        #if defined CONTROL_SENSOR
          // indicate with 4 flashes that we would NOT calibrate
          sensor_set_flash(0, 4);
          sensor_set_flash(1, 4);
        #endif
        }
      }
    }
  } else {

    // ONLY take action if the startup button was NOT down when we started up, or has been released since
    if (!power_button_info.startup_button_held) {
      // if this is a button release
      if (power_button_info.button_prev) {
        // power button held for < 100ms or > 10s -> nothing
        if ((power_button_info.button_held_ms >= 15000) || (power_button_info.button_held_ms < 100))
        {
          // no action taken
        }

        // power button held for between 5s and 10s -> HB angle calibration
        // (only if it had been released since startup)
        if ((power_button_info.button_held_ms >= 10000) &&
            (power_button_info.button_held_ms < 15000))
        {
          buzzerPattern = 0;
          enable = 0;

        #if defined CONTROL_SENSOR
          // indicate we accepted calibrate command
          sensor_set_flash(0, 8);
          sensor_set_flash(1, 8);
        #endif

          // buz to indicate we are calibrating
          for (int i = 0; i < 20; i++) {
            buzzerFreq = i & 3;
            HAL_Delay(100);
          }

        #if defined CONTROL_SENSOR && defined FLASH_STORAGE
          setUSART2ToControl();
          consoleLog("*** Write Flash Calibration data\r\n");
        #else
          consoleLog("*** Not a hoverboard, not modifyiing USART2\r\n");
        #endif

        }

        // power button held for between 5s and 10s -> HB angle calibration
        // (only if it had been released since startup)
        if ((power_button_info.button_held_ms >= 5000) &&
            (power_button_info.button_held_ms < 10000))
        {
          buzzerPattern = 0;
          enable = 0;

        #if defined CONTROL_SENSOR
          // indicate we accepted calibrate command
          sensor_set_flash(0, 8);
          sensor_set_flash(1, 8);
        #endif

          // buz to indicate we are calibrating
          for (int i = 0; i < 20; i++) {
            buzzerFreq = i & 2;
            HAL_Delay(100);
          }

        #if defined CONTROL_SENSOR && defined FLASH_STORAGE
          // read current board angles, and save to flash as center
          FlashContent.calibration_0 = sensor_data[0].Center_calibration = sensor_data[0].complete.Angle;
          FlashContent.calibration_1 = sensor_data[1].Center_calibration = sensor_data[1].complete.Angle;
          writeFlash( (unsigned char *)&FlashContent, sizeof(FlashContent) );
          consoleLog("*** Write Flash Calibration data\r\n");
        #else
          consoleLog("*** Not a hoverboard, no calibrarion done\r\n");
        #endif

        }

        // power button held for >100ms < 2s -> power off
        // (only if it had been released since startup)
        if ((power_button_info.button_held_ms >= 100) &&
            (power_button_info.button_held_ms < 2000)) {
          enable = 0;
          //while (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN));
          consoleLog("power off by button\r\n");
          poweroff();
        }

        // always stop flash after done/released regardless
      #if defined CONTROL_SENSOR
        sensor_set_flash(0, 0);
        sensor_set_flash(1, 0);
      #endif

      } // end of if power button was previous down
    }

    // always mark the button as released
    power_button_info.button_held_ms = 0;
    power_button_info.button_prev = 0;
    power_button_info.startup_button_held = 0;
  } // end of else power button must be up
}



/** System Clock Configuration
*/
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct;
  RCC_ClkInitTypeDef RCC_ClkInitStruct;
  RCC_PeriphCLKInitTypeDef PeriphClkInit;

  /**Initializes the CPU, AHB and APB busses clocks
    */
  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = 16;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL          = RCC_PLL_MUL16;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  /**Initializes the CPU, AHB and APB busses clocks
    */
  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection    = RCC_ADCPCLK2_DIV8;  // 8 MHz
  HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit);

  /**Configure the Systick interrupt time
    */
  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq() / 1000);

  /**Configure the Systick
    */
  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 1, 0);

  // enable the DWT counter for cycle timing
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

}
