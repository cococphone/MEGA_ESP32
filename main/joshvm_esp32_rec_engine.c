#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"
#include "board.h"
#include "audio_common.h"
#include "audio_pipeline.h"
#include "mp3_decoder.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "filter_resample.h"
#include "esp_sr_iface.h"
#include "esp_sr_models.h"
#include "sdkconfig.h"
#include "audio_mem.h"
#include "esp_audio.h"
#include "audio_element.h"
#include "esp_vad.h"

#include "joshvm_esp32_rec_engine.h"
#include "joshvm_esp32_media.h"
#include "joshvm.h"
#include "joshvm_esp32_ring_buff.h"

#include "recorder_engine.h"
extern UBaseType_t pvCreatedTask_vadtask;//test

//---define
#define VAD_SAMPLE_RATE_HZ 16000
#define VAD_FRAME_LENGTH_MS 30
#define VAD_BUFFER_LENGTH (VAD_FRAME_LENGTH_MS * VAD_SAMPLE_RATE_HZ / 1000)
#define VAD_OFF_TIME 800
#define pause_resume_flag_resume 0
#define pause_resume_flag_pause 1


//---enum
typedef enum {
    WAKE_UP = 1,
    OPEN_THE_LIGHT,
    CLOSE_THE_LIGHT,
    VOLUME_INCREASE,
    VOLUME_DOWN,
    PLAY,
    PAUSE,
    MUTE,
    PLAY_LOCAL_MUSIC,
} asr_event_e;

typedef enum{
	EVENT_WAKEUP_START = 0,
	EVENT_VAD_START,
	EVENT_VAD_STOP,
	EVENT_WAKEUP_END,
}rec_event_tpye_e;

typedef enum{
	STATE_NULL = 0,
	WAKEUP_ENABLE,
	WAKEUP_DISABLE,
	VAD_START,
	VAD_PAUSE,
	VAD_RESUME,
	VAD_STOP,
}rec_status_e;

//---struct
typedef struct{
	int8_t wakeup_state;
	int8_t vad_state;
	int8_t pause_resume_flag;
	uint32_t vad_off_time;
	void(*wakeup_callback)(int);//notify jvm
	void(*vad_callback)(int);//notify jvm
}rec_engine_t;

//---variable
static const char *TAG = "JOSHVM_REC_ENGINE>>>>>>>";
rec_engine_t rec_engine  = {WAKEUP_DISABLE,VAD_STOP,pause_resume_flag_resume,1000,NULL,NULL};
static int8_t need_notify_vad_stop = false;
static uint16_t que_val = 0;
static int8_t task_run =1;
static QueueHandle_t vad_que = NULL;
extern joshvm_media_t *joshvm_media_vad;
uint32_t vad_off_time = 0;



static void rec_engine_task(void *handle)
{
	vad_que = xQueueCreate(4, sizeof(uint16_t));	
	const esp_sr_iface_t *model = &esp_sr_wakenet5_quantized;
	ESP_LOGE(TAG,"11before wakeup iram free_size = %d\r\n%s\n",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),__FILE__);
	model_iface_data_t *iface = model->create(DET_MODE_90);
	ESP_LOGE(TAG,"11after wakeup iram free_size = %d\r\n%s\n",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),__FILE__);
	int audio_chunksize = model->get_samp_chunksize(iface);
	audio_chunksize = audio_chunksize * sizeof(short);
	rec_engine_t* rec_engine = (rec_engine_t*)handle;
	vad_state_t last_vad_state = 0;
	vad_state_t vad_state = 0;
	int8_t vad_writer_buff_flag = 0;
	uint32_t written_size = 0;
	int16_t *buff = (int16_t *)malloc(audio_chunksize * sizeof(short));
	if (NULL == buff) {
		ESP_LOGE(TAG, "Memory allocation failed!");
		model->destroy(iface);
		model = NULL;
		return;
	}

	audio_pipeline_handle_t pipeline;
	audio_element_handle_t i2s_stream_reader, filter, raw_read;

	audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
	pipeline = audio_pipeline_init(&pipeline_cfg);
	mem_assert(pipeline);

	i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
	i2s_cfg.i2s_config.sample_rate = 48000;
	i2s_cfg.type = AUDIO_STREAM_READER;
//#if defined CONFIG_ESP_LYRAT_MINI_V1_1_BOARD
#if defined CONFIG_ESP_LYRATD_MINI_V1_1_BOARD

	i2s_cfg.i2s_port = 1;
#endif
	i2s_stream_reader = i2s_stream_init(&i2s_cfg);

	rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
	rsp_cfg.src_rate = 48000;
	rsp_cfg.src_ch = 2;
	rsp_cfg.dest_rate = 16000;
	rsp_cfg.dest_ch = 1;
	rsp_cfg.type = AUDIO_CODEC_TYPE_ENCODER;
	filter = rsp_filter_init(&rsp_cfg);

	raw_stream_cfg_t raw_cfg = {
		.out_rb_size = 8 * 1024,
		.type = AUDIO_STREAM_READER,
	};
	raw_read = raw_stream_init(&raw_cfg);

	audio_pipeline_register(pipeline, i2s_stream_reader, "i2s_rec_engine");
	audio_pipeline_register(pipeline, filter, "filter_rec_engine");
	audio_pipeline_register(pipeline, raw_read, "raw_rec_engine");
	audio_pipeline_link(pipeline, (const char *[]) {"i2s_rec_engine", "filter_rec_engine", "raw_rec_engine"}, 3);
	audio_pipeline_run(pipeline);
	ESP_LOGE(TAG,"before vad iram free_size = %d\r\n%s\n",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),__FILE__);
	vad_handle_t vad_inst = vad_create(VAD_MODE_3, VAD_SAMPLE_RATE_HZ, VAD_FRAME_LENGTH_MS);
	ESP_LOGE(TAG,"after vad iram free_size = %d\r\n%s\n",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),__FILE__);
	task_run = 1;
	while (task_run) {
		raw_stream_read(raw_read, (char *)buff, audio_chunksize);
	
		if((rec_engine->vad_state == VAD_START) || (rec_engine->vad_state == VAD_RESUME)){
			vad_state = vad_process(vad_inst, buff);
			
			//clear timer,vad_off_time increase 1 per 200ms
			if(vad_state == VAD_SPEECH){				
				vad_off_time = 0;
			}			
				
			//detect voice 
			if((vad_state != last_vad_state) && (vad_state == VAD_SPEECH) && (vad_writer_buff_flag == 0)){
				que_val = VAD_START;
				xQueueSend(vad_que, &que_val, portMAX_DELAY);				
				last_vad_state = vad_state;	
				vad_writer_buff_flag = 1;	
				joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.rb_callback_flag = NO_NEED_CB;//init
			}else if((vad_state != last_vad_state) && (vad_state == VAD_SILENCE)){
				last_vad_state = vad_state;
				need_notify_vad_stop = true;
			}
			//vad stop		
			if(((vad_off_time * 200) >= rec_engine->vad_off_time) && (need_notify_vad_stop == true)){				
				ESP_LOGI(TAG,"vad_off_time = %d,need_notify_vad_stop = %d,rec_engine->vad_state = %d",vad_off_time * 200,need_notify_vad_stop,rec_engine->vad_state);
				que_val = VAD_STOP;
				xQueueSend(vad_que, &que_val, portMAX_DELAY);				
			}
			//save voice data
			if(vad_writer_buff_flag){
				written_size = ring_buffer_write(buff,audio_chunksize,joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.rec_rb,RB_COVER);
				if((written_size) && (NEED_CB == joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.rb_callback_flag)){
					joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.rb_callback_flag = NO_NEED_CB;
					joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.rb_callback(joshvm_media_vad,JOSHVM_OK);
				}
			}
		}

		xQueueReceive(vad_que, &que_val, 0);
		if(que_val == VAD_START){
			ESP_LOGI(TAG,"VAD_START");
			que_val = STATE_NULL;
			xQueueSend(vad_que, &que_val, portMAX_DELAY);
			rec_engine->vad_callback(0);			
			
		}else if(que_val == VAD_PAUSE){
			if(need_notify_vad_stop == true){
				ESP_LOGI(TAG,"VAD_PAUSE");
				que_val = STATE_NULL;
				xQueueSend(vad_que, &que_val, portMAX_DELAY);
				rec_engine->vad_callback(1);				
				need_notify_vad_stop = false;
				vad_writer_buff_flag = 0;
			}else if(need_notify_vad_stop == false){
				ESP_LOGI(TAG,"VAD_PAUSE,but vad has stopped");
				que_val = STATE_NULL;
				xQueueSend(vad_que, &que_val, portMAX_DELAY);
			}			
		}else if(que_val == VAD_RESUME){
			ESP_LOGI(TAG,"VAD_RESUME");
			rec_engine->vad_state = VAD_RESUME;
			que_val = STATE_NULL;
			xQueueSend(vad_que, &que_val, portMAX_DELAY);
			
		}else if(que_val == VAD_STOP){
			ESP_LOGI(TAG,"VAD_STOP");
			que_val = STATE_NULL;
			xQueueSend(vad_que, &que_val, portMAX_DELAY);
			rec_engine->vad_callback(1);
			joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.status = AUDIO_STOP;
			need_notify_vad_stop = false;	
			vad_writer_buff_flag = 0;	
			
		}		

		if(rec_engine->wakeup_state == WAKEUP_ENABLE){
			int keyword = model->detect(iface, (int16_t *)buff);
			switch (keyword) {
				case WAKE_UP:
					ESP_LOGI(TAG, "Wake up");					
					printf("vad task free stack :				%d\n",pvCreatedTask_vadtask);				
					printf("xPortGetMinimumEverFreeHeapSize :   %d\n",xPortGetMinimumEverFreeHeapSize());				
					ESP_LOGE(TAG,"wakeup iram free_size = %d\r\n%s\n",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),__FILE__);
					//extern void joshvm_spiffs_audio_play_handler(const char *url);
					//joshvm_spiffs_audio_play_handler("/userdata/ding.mp3");
				
					if(rec_engine->wakeup_callback != NULL){
						rec_engine->wakeup_callback(0);
					}
					break;
				default:
					ESP_LOGD(TAG, "Not supported keyword");
					break;
			}
		}
	}
    ESP_LOGI(TAG, "[ 5 ] Destroy VAD");
    vad_destroy(vad_inst);

	ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");	
	audio_pipeline_terminate(pipeline);	
	/* Terminate the pipeline before removing the listener */
	audio_pipeline_remove_listener(pipeline);	
	audio_pipeline_unregister(pipeline, raw_read);
	audio_pipeline_unregister(pipeline, i2s_stream_reader);
	audio_pipeline_unregister(pipeline, filter);	
	/* Release all resources */
	audio_pipeline_deinit(pipeline);
	audio_element_deinit(raw_read);
	audio_element_deinit(i2s_stream_reader);
	audio_element_deinit(filter);

	ESP_LOGI(TAG, "[ 7 ] Destroy model");
	model->destroy(iface);
	model = NULL;
	free(buff);
	buff = NULL;	
	if(vad_que != NULL){
		vQueueDelete(vad_que);
		vad_que = NULL;
	}	
	vTaskDelete(NULL);
}

static esp_err_t joshvm_rec_engine_create(rec_engine_t* rec_engine,rec_status_e type)
{
	int8_t wakeup_state,vad_state;
	wakeup_state = rec_engine->wakeup_state;
	vad_state = rec_engine->vad_state;

	switch(type){
		case WAKEUP_ENABLE:
			rec_engine->wakeup_state = WAKEUP_ENABLE;
		break;
		case VAD_START:
			rec_engine->vad_state = VAD_START;
			rec_engine->pause_resume_flag = pause_resume_flag_resume;//init this flag every vadstart for pause 
		break;
		default :

		break;
	}

	if((wakeup_state == WAKEUP_ENABLE) || (vad_state == VAD_START)){
		ESP_LOGI(TAG,"rec_engine have created!");
		return JOSHVM_OK;
	}	
	
	ESP_LOGE(TAG,"before wakeup  heap free_size = %d,%s,%d",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),__FILE__,__LINE__);
	xTaskCreate(rec_engine_task, "rec_engine_task",4*1024, rec_engine, 20, NULL);

	return 0;
}

esp_err_t joshvm_rec_engine_destroy(rec_engine_t* rec_engine,rec_status_e type)
{
	switch(type){
		case WAKEUP_DISABLE:
			rec_engine->wakeup_state = WAKEUP_DISABLE;
		break;
		case VAD_STOP:
			rec_engine->vad_state = VAD_STOP;
		break;
		default:

		break;
	}

	if((rec_engine->wakeup_state == WAKEUP_DISABLE) && (rec_engine->vad_state == VAD_STOP)){
		task_run = 0;
	}
	
	return 0;
}



//---------------------------------------------
int joshvm_esp32_wakeup_get_word_count(void)
{	
	return 1;
}

int joshvm_esp32_wakeup_get_word(int pos, int* index, char* wordbuf, int wordlen, char* descbuf, int desclen)
{
	*index = 0;
	memcpy(wordbuf,"Hi LeXin",sizeof("Hi LeXin"));	
	memcpy(descbuf,"enjoy happy",sizeof("enjoy happy"));

	return 0;
}

int joshvm_esp32_wakeup_enable(void(*callback)(int))
{
    if(rec_engine.wakeup_state == WAKEUP_ENABLE){
		ESP_LOGW(TAG,"wakeup has already enable");
		return JOSHVM_INVALID_STATE;
	}

	int8_t ret;
	extern int8_t create_cnt;
	if(create_cnt == 0){
		if(joshvm_esp32_audio_board_init() != JOSHVM_OK){
			ESP_LOGI(TAG,"AUDIO BOARD INIT FAIL!");
			return JOSHVM_FAIL;
		}
		create_cnt++;
		vTaskDelay(200);
	}
	
	ESP_LOGI(TAG,"joshvm_esp32_wakeup_enable");
	rec_engine.wakeup_callback = callback;
	ret = joshvm_rec_engine_create(&rec_engine,WAKEUP_ENABLE);

	return ret;
}

int joshvm_esp32_wakeup_disable()
{
    if(rec_engine.wakeup_state == WAKEUP_DISABLE){
		ESP_LOGW(TAG,"Can't disable wakeup when have not enable");
		return JOSHVM_INVALID_STATE;
	}
	ESP_LOGI(TAG,"joshvm_esp32_wakeup_disable");
	int8_t ret;
	ret = joshvm_rec_engine_destroy(&rec_engine,WAKEUP_DISABLE);		
	return ret;
}

int joshvm_esp32_vad_start(void(*callback)(int))
{	
    if(rec_engine.vad_state == VAD_START){
		ESP_LOGW(TAG,"vad has already start");
		return JOSHVM_INVALID_STATE;
	}

	if(joshvm_media_vad == NULL){
		ESP_LOGI(TAG,"Vad obj have't created!");
		return JOSHVM_FAIL;
	}

	ESP_LOGI(TAG,"joshvm_esp32_vad_start");
	ring_buffer_flush(joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.rec_rb);
	printf("vad ringbuffer data valid size = %d\n",joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.rec_rb->valid_size);
	joshvm_media_vad->joshvm_media_u.joshvm_media_audio_vad_rec.status = AUDIO_START;	
	int8_t ret;
	rec_engine.vad_off_time = VAD_OFF_TIME;
	rec_engine.vad_callback = callback;
	ret = joshvm_rec_engine_create(&rec_engine,VAD_START);

	return ret;
}

int joshvm_esp32_vad_pause()
{
	printf("joshvm_esp32_vad_pause-------------\n");

	if(rec_engine.pause_resume_flag == pause_resume_flag_resume){
		rec_engine.pause_resume_flag = pause_resume_flag_pause;
		rec_engine.vad_state = VAD_PAUSE;
		que_val = VAD_PAUSE;
		xQueueSend(vad_que, &que_val, portMAX_DELAY);
	}
	return JOSHVM_OK;
}

int joshvm_esp32_vad_resume()
{
	printf("joshvm_esp32_vad_resume-----------------\n");
	if(rec_engine.pause_resume_flag == pause_resume_flag_pause){
		rec_engine.pause_resume_flag = pause_resume_flag_resume;
		rec_engine.vad_state = VAD_RESUME;
		que_val = VAD_RESUME;
		xQueueSend(vad_que, &que_val, portMAX_DELAY);
	}
	return JOSHVM_OK;
}

int joshvm_esp32_vad_stop()
{
    if(rec_engine.vad_state == VAD_STOP){
		ESP_LOGW(TAG,"Can't stop vad when have not start");
		return JOSHVM_INVALID_STATE;
	}

	ESP_LOGI(TAG, " joshvm_esp32_vad_stop"); 	
	joshvm_rec_engine_destroy(&rec_engine,VAD_STOP);
	return JOSHVM_OK;
}

int joshvm_esp32_vad_set_timeout(int ms)
{
	rec_engine.vad_off_time = ms;
	return JOSHVM_OK;
}

//--test-------------------

void test_callback(int index)
{
	printf("wakeup   callback  index = %d\n",index);
	
}

void test_vad_callback(int index)
{
	printf("vad   callback  ---------------- index = %d\n",index);
	
}

void test_rec_engine(void)
{

	joshvm_esp32_wakeup_enable(test_callback);

	joshvm_esp32_vad_start(test_vad_callback);


}

