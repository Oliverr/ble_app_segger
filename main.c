/* Copyright (c) 2014 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */
/** @example examples/ble_peripheral/ble_app_hrs/main.c
 *
 * @brief Heart Rate Service Sample Application main file.
 *
 * This file contains the source code for a sample application using the Heart Rate service
 * (and also Battery and Device Information services). This application uses the
 * @ref srvlib_conn_params module.
 */

#include <stdint.h>
#include <string.h>
#include "nordic_common.h"
#include "nrf.h"
#include "app_error.h"
#include "ble.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_advertising.h"
#include "ble_bas.h"
#include "ble_hrs.h"
#include "ble_dis.h"
#ifdef BLE_DFU_APP_SUPPORT
#include "ble_dfu.h"
#include "dfu_app_handler.h"
#endif // BLE_DFU_APP_SUPPORT
#include "ble_conn_params.h"
#include "boards.h"
#include "sensorsim.h"
#include "softdevice_handler.h"
#include "app_timer.h"
#include "device_manager.h"
#include "pstorage.h"
#include "app_trace.h"
#include "bsp.h"
#include "nrf_delay.h"
#include "bsp_btn_ble.h"
#include "twi_master.h"
#include "nrf_gpio.h"

#include "SEGGER_RTT.h"

#define TMP102_ADDRESS 	0x90

#define LED_RED       	21
#define LED_AMB       	23
#define LED_GRN       	22
#define SW_LED					2
#define SW_TEST   			1
#define SW_WAKE   			17

#define IS_SRVC_CHANGED_CHARACT_PRESENT 0                                 			/**< Include or not the service_changed characteristic. if not enabled, the server's database cannot be changed for the lifetime of the device*/

#define DEVICE_NAME                     "EMON_123456789"

#define APP_CFG_NON_CONN_ADV_TIMEOUT    0                                 			/**< Time for which the device must be advertising in non-connectable mode (in seconds). 0 disables timeout. */
#define NON_CONNECTABLE_ADV_INTERVAL    MSEC_TO_UNITS(100, UNIT_0_625_MS) 			/**< The advertising interval for non-connectable advertisement (100 ms). This value can vary between 100ms to 10.24s). */

#define MIN_CONN_INTERVAL               MSEC_TO_UNITS(100, UNIT_1_25_MS)       /**< Minimum acceptable connection interval (0.1 seconds). */
#define MAX_CONN_INTERVAL               MSEC_TO_UNITS(200, UNIT_1_25_MS)       /**< Maximum acceptable connection interval (0.2 second). */
#define SLAVE_LATENCY                   0                                      /**< Slave latency. */
#define CONN_SUP_TIMEOUT                MSEC_TO_UNITS(4000, UNIT_10_MS)        /**< Connection supervisory timeout (4 seconds). */
#define APP_ADV_INTERVAL                10000                                   /**< The advertising interval (in units of 0.625 ms. This value corresponds to 1 sec). */
#define APP_CFG_CONNECTION_INTERVAL   	10000		// new

#define APP_ADV_TIMEOUT_IN_SECONDS      30 //was 180                           /**< The advertising timeout in units of seconds. */

#define SEC_PARAM_BOND                  1                                      /**< Perform bonding. */
#define SEC_PARAM_MITM                  0                                      /**< Man In The Middle protection not required. */
#define SEC_PARAM_IO_CAPABILITIES       BLE_GAP_IO_CAPS_NONE                   /**< No I/O capabilities. */
#define SEC_PARAM_OOB                   0                                      /**< Out Of Band data not available. */
#define SEC_PARAM_MIN_KEY_SIZE          7                                      /**< Minimum encryption key size. */
#define SEC_PARAM_MAX_KEY_SIZE          16                                     /**< Maximum encryption key size. */

#define DEAD_BEEF                       0xDEADBEEF

#define START_STRING                    "Start...\n"

/**< Value used as error code on stack dump, can be used to identify stack location on stack unwind. */

static uint32_t m_app_ticks_per_100ms = 0;

#define APP_TIMER_PRESCALER             0                                 			/**< Value of the RTC1 PRESCALER register. */
#define APP_TIMER_OP_QUEUE_SIZE         4                                 			/**< Size of timer operation queues. */
// Timeout before the first measurement. DHT-22 needs at least 1 second warmup time
#define WARMUP_INTERVAL   							100
// Timeout between the measurements.
#define SAMPLER_INTERVAL                10000
#define BSP_MS_TO_TICK(MS) (m_app_ticks_per_100ms * (MS / 100))

#define EMON_TEMPERATURE_SERVICE_UUID 	0x7B18
ble_advdata_service_data_t EMON_service_data;                         // Structure to hold Service Data.
static ble_advdata_t advdata;
#define EMON_SERVICE_DATA_ARRAY_LENGTH  2 //4 -> 2 /* 2 bytes for humidity, 2 bytes for temperature */
static uint8_t EMON_service_data_array[EMON_SERVICE_DATA_ARRAY_LENGTH] =
{
								0xFF,0xFF //,0xFF,0xFF,
								//0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

//static ble_uuid_t m_adv_uuids[] = {}; /**< Universally unique service identifiers. */

static dm_application_instance_t m_app_handle;                                      /**< Application identifier allocated by device manager */
APP_TIMER_DEF(m_sampler_timer_id);
APP_TIMER_DEF(m_warmup_timer_id);

/**@brief Callback function for asserts in the SoftDevice.
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyze
 *          how your product is supposed t\o react in case of Assert.
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 *
 * @param[in]   line_num   Line number of the failing ASSERT call.
 * @param[in]   file_name  File name of the failing ASSERT call.
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
								app_error_handler(DEAD_BEEF, line_num, p_file_name);
}

/**@brief Function for putting the chip into sleep mode.
 *
 * @note This function will not return.
 */
static void sleep_mode_enter(void)
{
								uint32_t err_code = bsp_indication_set(BSP_INDICATE_IDLE);
								APP_ERROR_CHECK(err_code);

								// Prepare wakeup buttons.
								err_code = bsp_btn_ble_sleep_mode_prepare();
								APP_ERROR_CHECK(err_code);

								// Go to system-off mode (this function will not return; wakeup will cause a reset).
								err_code = sd_power_system_off();
								APP_ERROR_CHECK(err_code);
}

/**@brief Function for the GAP initialization.
 *
 * @details This function sets up all the necessary GAP (Generic Access Profile) parameters of the
 *          device including the device name, appearance, and the preferred connection parameters.
 */
static void gap_params_init(void)
{
								uint32_t err_code;
								ble_gap_conn_params_t gap_conn_params;
								ble_gap_conn_sec_mode_t sec_mode;

								BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

								err_code = sd_ble_gap_device_name_set(&sec_mode,(const uint8_t *)DEVICE_NAME,strlen(DEVICE_NAME));
								APP_ERROR_CHECK(err_code);

								err_code = sd_ble_gap_appearance_set(BLE_APPEARANCE_GENERIC_THERMOMETER);
								APP_ERROR_CHECK(err_code);

								memset(&gap_conn_params, 0, sizeof(gap_conn_params));

								gap_conn_params.min_conn_interval = MIN_CONN_INTERVAL;
								gap_conn_params.max_conn_interval = MAX_CONN_INTERVAL;
								gap_conn_params.slave_latency     = SLAVE_LATENCY;
								gap_conn_params.conn_sup_timeout  = CONN_SUP_TIMEOUT;

								err_code = sd_ble_gap_ppcp_set(&gap_conn_params);
								APP_ERROR_CHECK(err_code);
}

/**@brief Function for handling advertising events.
 *
 * @details This function will be called for advertising events which are passed to the application.
 *
 * @param[in] ble_adv_evt  Advertising event.
 */
static void on_adv_evt(ble_adv_evt_t ble_adv_evt)
{
								uint32_t err_code;

								switch (ble_adv_evt)
								{
								case BLE_ADV_EVT_FAST:
																err_code = bsp_indication_set(BSP_INDICATE_ADVERTISING);
																APP_ERROR_CHECK(err_code);
																break;
								case BLE_ADV_EVT_IDLE:
																sleep_mode_enter();
																break;
								default:
																break;
								}
}


/**@brief Function for handling the Device Manager events.
 *
 * @param[in] p_evt  Data associated to the device manager event.
 */
static uint32_t device_manager_evt_handler(dm_handle_t const * p_handle,dm_event_t const  * p_event,ret_code_t event_result)
{
								APP_ERROR_CHECK(event_result);

								#ifdef BLE_DFU_APP_SUPPORT
								if (p_event->event_id == DM_EVT_LINK_SECURED)
								{
																app_context_load(p_handle);
								}
								#endif // BLE_DFU_APP_SUPPORT

								return NRF_SUCCESS;
}

/**@brief Function for the Device Manager initialization.
 *
 * @param[in] erase_bonds  Indicates whether bonding information should be cleared from
 *                         persistent storage during initialization of the Device Manager.
 */
static void device_manager_init(bool erase_bonds)
{
								uint32_t err_code;
								dm_init_param_t init_param = {.clear_persistent_data = erase_bonds};
								dm_application_param_t register_param;

								// Initialize persistent storage module.
								err_code = pstorage_init();
								APP_ERROR_CHECK(err_code);

								err_code = dm_init(&init_param);
								APP_ERROR_CHECK(err_code);

								memset(&register_param.sec_param, 0, sizeof(ble_gap_sec_params_t));

								register_param.sec_param.bond         = SEC_PARAM_BOND;
								register_param.sec_param.mitm         = SEC_PARAM_MITM;
								register_param.sec_param.io_caps      = SEC_PARAM_IO_CAPABILITIES;
								register_param.sec_param.oob          = SEC_PARAM_OOB;
								register_param.sec_param.min_key_size = SEC_PARAM_MIN_KEY_SIZE;
								register_param.sec_param.max_key_size = SEC_PARAM_MAX_KEY_SIZE;
								register_param.evt_handler            = device_manager_evt_handler;
								register_param.service_type           = DM_PROTOCOL_CNTXT_GATT_SRVR_ID;

								err_code = dm_register(&m_app_handle, &register_param);
								APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing the Advertising functionality.
 */
static void advertising_init(void)
{
								uint32_t err_code;

// Our service data entry which will also contain the sensor data
								EMON_service_data.service_uuid = EMON_TEMPERATURE_SERVICE_UUID;
								EMON_service_data.data.p_data = (uint8_t *)EMON_service_data_array;                 // Array for service advertisement data.
								EMON_service_data.data.size = EMON_SERVICE_DATA_ARRAY_LENGTH;

// Build advertising data struct to pass into @ref ble_advertising_init.
								memset(&advdata, 0, sizeof(advdata));

								advdata.name_type               = BLE_ADVDATA_FULL_NAME;
								advdata.include_appearance      = true;
								advdata.flags                   = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
								//advdata.uuids_complete.uuid_cnt = sizeof(m_adv_uuids) / sizeof(m_adv_uuids[0]);
								//advdata.uuids_complete.p_uuids  = m_adv_uuids;
								advdata.uuids_complete.uuid_cnt = 0;
								advdata.p_service_data_array    = &EMON_service_data;          // Pointer to Service Data structure.
								advdata.service_data_count      = 1;

								ble_adv_modes_config_t options = {0};
								options.ble_adv_fast_enabled  = BLE_ADV_FAST_ENABLED;
								options.ble_adv_fast_interval = APP_ADV_INTERVAL;
								options.ble_adv_fast_timeout  = APP_ADV_TIMEOUT_IN_SECONDS;

								err_code = ble_advertising_init(&advdata, NULL, &options, on_adv_evt, NULL);
								APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing the BLE stack.
 *
 * @details Initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
								uint32_t err_code;

								//nrf_clock_lf_cfg_t clock_lf_cfg = NRF_CLOCK_LFCLKSRC;
								// Initialize the SoftDevice handler module.
								SOFTDEVICE_HANDLER_INIT(NRF_CLOCK_LFCLKSRC_XTAL_20_PPM, NULL);
								//SOFTDEVICE_HANDLER_INIT(&clock_lf_cfg, NULL);

								// Enable BLE stack
								ble_enable_params_t ble_enable_params;
								memset(&ble_enable_params, 0, sizeof(ble_enable_params));
#if (defined(S130) && defined(S132))
								ble_enable_params.gatts_enable_params.attr_tab_size   = BLE_GATTS_ATTR_TAB_SIZE_DEFAULT;
#endif
								ble_enable_params.gatts_enable_params.service_changed = IS_SRVC_CHANGED_CHARACT_PRESENT;
								err_code = sd_ble_enable(&ble_enable_params);
								APP_ERROR_CHECK(err_code);
}

static uint32_t read_temp_info(){

								uint8_t write_buffer[3];
								uint8_t data_buffer[2];

								nrf_gpio_pin_set(LED_GRN);

								twi_master_init();
								// Trigger a one-shot conversion from the temperature sensor
								write_buffer[0]=1; // i.e. Configuration register
								write_buffer[1]=129; // MSB 128=OS bit, 1=SD bit
								write_buffer[2]=0; // LSB

								if(twi_master_transfer(TMP102_ADDRESS, write_buffer, 3, TWI_ISSUE_STOP)) {
								}

								// Wait for OS bit to be re-set to 1 to signal end of conversion
								data_buffer[0]=0;
								while (data_buffer[0] && 128!=128) {
																if (twi_master_transfer(TMP102_ADDRESS | TWI_READ_BIT, data_buffer, 2, TWI_ISSUE_STOP)) {
																}
								}

								// Switch to temperature register (from config register)
								write_buffer[0]=0; // i.e. Temperature register
								write_buffer[1]=0; // MSB?
								write_buffer[2]=0; // LSB?
								if(twi_master_transfer(TMP102_ADDRESS, write_buffer, 3, TWI_ISSUE_STOP)) {
								}

								// Read the temperature and place in the payload ready to send
								if (twi_master_transfer(TMP102_ADDRESS | TWI_READ_BIT, data_buffer, 2, TWI_ISSUE_STOP)) {

																SEGGER_RTT_WriteString(0, "New reading: ");
																SEGGER_RTT_printf(0, "%02X ", data_buffer[0]);
																SEGGER_RTT_printf(0, "%02X", data_buffer[1]);
																SEGGER_RTT_WriteString(0, "\n");
																memcpy(EMON_service_data_array,data_buffer,5);

								}

								// Prepare data that will put TMP sensor into shutdown
								write_buffer[0]=1; // i.e. Configuration register
								write_buffer[1]=1; // MSB? 1=SHUTDOWN
								write_buffer[2]=0; // LSB?

								if(twi_master_transfer(TMP102_ADDRESS, write_buffer, 3, TWI_ISSUE_STOP)) {
								}

								// Disable TWI ready for sleep

								NRF_TWI1->ENABLE= TWI_ENABLE_ENABLE_Disabled << TWI_ENABLE_ENABLE_Pos;

								nrf_gpio_pin_clear(LED_GRN);

								return 1;

}

/*static uint32_t refresh_temp_info() {
   int dht22decode( int pin, unsigned char data_bits[] );
   int rsp,i,retryCtr;
   unsigned char dht_data[5];

   retryCtr = 1;
   while( retryCtr > 0 ) {
   rsp = dht22decode(DHT22_PIN,dht_data);
   if( !rsp ) {
   //UART0_print("Error decoding\r\n");
   SEGGER_RTT_WriteString(0, "Error decoding\n");
   --retryCtr;
   } else {
   for( i = 0 ; i < 5 ; ++i ) {
   //UART0_printHex2(dht_data[i]);
   SEGGER_RTT_printf(0, "%x ", dht_data[i]);
   //SEGGER_RTT_printf(0, %s, dht_data[i]);
   //UART0_print(" " );
   }
   //memcpy( dht22_service_data_array,dht_data,4 );
   memcpy( dht22_service_data_array,"11.1",4);

   //SEGGER_RTT_WriteString(0, dht22_service_data);
   SEGGER_RTT_WriteString(0, "\n");
   //UART0_println();
   break;
   }
   }
   return retryCtr > 0 ? 1 : 0;
   }*/

/**@brief Handle events from sampler timer.
 *
 * @param[in]   p_context   parameter registered in timer start function.
 */
static void sampler_timer_handler(void * p_context)
{
								uint32_t err_code,status;

								UNUSED_PARAMETER(p_context);
								status = read_temp_info();
								err_code = sd_ble_gap_adv_stop();
								APP_ERROR_CHECK(err_code);
								if( status ) {
																err_code = ble_advdata_set(&advdata, NULL);
																APP_ERROR_CHECK(err_code);
																err_code = ble_advertising_start(BLE_ADV_MODE_FAST);
																APP_ERROR_CHECK(err_code);
								}
}

/**@brief Handle the warmup timer event.
 * DHT22 needs at least 1 sec of warmup before it can start its first measurement. Warmup timer
 * is a one-shot timer that waits for the warmup time before finishing the system initialization
 *
 * @param[in]   p_context   parameter registered in timer start function.
 */
static void warmup_timer_handler(void * p_context)
{
								uint32_t err_code;
								char warmupDoneMsg[] = "Warmup done\n";
								char samplingStartedMsg[] = "Sampling starting\n";

								SEGGER_RTT_WriteString(0, samplingStartedMsg);

								UNUSED_PARAMETER(p_context);
								SEGGER_RTT_WriteString(0, warmupDoneMsg);

								err_code = read_temp_info();
								if( err_code ) {
																err_code = ble_advdata_set(&advdata, NULL);
																APP_ERROR_CHECK(err_code);
																err_code = ble_advertising_start(BLE_ADV_MODE_FAST);
																APP_ERROR_CHECK(err_code);
								}
								err_code = app_timer_start(m_sampler_timer_id, BSP_MS_TO_TICK(SAMPLER_INTERVAL), NULL);
								APP_ERROR_CHECK(err_code);

								//SEGGER_RTT_WriteString(0, "Sampling started");
}

static void timers_init(void)
{
								uint32_t err_code;

								// Initialize timer module.
								APP_TIMER_INIT(APP_TIMER_PRESCALER, APP_TIMER_OP_QUEUE_SIZE, false);
								m_app_ticks_per_100ms = APP_TIMER_TICKS(100, APP_TIMER_PRESCALER);

								// Create timers.
								err_code = app_timer_create(&m_sampler_timer_id, APP_TIMER_MODE_REPEATED, sampler_timer_handler);
								APP_ERROR_CHECK(err_code);
								err_code = app_timer_create(&m_warmup_timer_id, APP_TIMER_MODE_SINGLE_SHOT, warmup_timer_handler);
								APP_ERROR_CHECK(err_code);
}

static void warmup_timer_start(void) {
								uint32_t err_code;
								err_code = app_timer_start(m_warmup_timer_id, BSP_MS_TO_TICK(WARMUP_INTERVAL), NULL);
								APP_ERROR_CHECK(err_code);
}

static void fill_deviceid(void) {
								uint32_t deviceid0,deviceid1;
								unsigned char *ptr;
								deviceid0 = NRF_FICR->DEVICEID[0];
								deviceid1 = NRF_FICR->DEVICEID[1];
								ptr = EMON_service_data_array+2; // 4-> 2
								*(ptr++) = (unsigned char)( deviceid0 & 0xFF );
								deviceid0 >>= 8;
								*(ptr++) = (unsigned char)( deviceid0 & 0xFF );
								deviceid0 >>= 8;
								*(ptr++) = (unsigned char)( deviceid0 & 0xFF );
								deviceid0 >>= 8;
								*(ptr++) = (unsigned char)( deviceid0 & 0xFF );
								deviceid0 >>= 8;
								*(ptr++) = (unsigned char)( deviceid1 & 0xFF );
								deviceid1 >>= 8;
								*(ptr++) = (unsigned char)( deviceid1 & 0xFF );
								deviceid1 >>= 8;
								*(ptr++) = (unsigned char)( deviceid1 & 0xFF );
								deviceid1 >>= 8;
								*(ptr++) = (unsigned char)( deviceid1 & 0xFF );
}

/**@brief Function for doing power management.
 */
static void power_manage(void)
{
								uint32_t err_code = sd_app_evt_wait();
								APP_ERROR_CHECK(err_code);
}



/**
 * @brief Function for application main entry.
 */
int main(void)
{
								uint32_t err_code;
								bool erase_bonds = false;
								char initMsg[] = "Init\n";
								char warmupMsg[] = "Warmup ...\n";

								// Initialize.
								SEGGER_RTT_WriteString(0, initMsg);
								fill_deviceid();
								timers_init();
								err_code = bsp_init(BSP_INIT_LED, m_app_ticks_per_100ms, NULL);
								APP_ERROR_CHECK(err_code);
								ble_stack_init();
								device_manager_init(erase_bonds);

								gap_params_init();
								advertising_init();
								warmup_timer_start();
								SEGGER_RTT_WriteString(0, warmupMsg);


								// Enter main loop.
								for (;; )
								{
																power_manage();
								}
}


/**
 * @}
 */
