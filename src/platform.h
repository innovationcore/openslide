/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2022  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include "common.h"
#include "mathutils.h"
#include "arena.h"
//#include "memrw.h"
//
//#include "keycode.h"
//#include "keytable.h"


#if WINDOWS
#include <windows.h>
#else
#include <semaphore.h>
#include <unistd.h>
#include <aio.h> // For async io
#include <errno.h> // For async io
#endif

#if APPLE
#include <stddef.h> // for offsetof()
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#elif LINUX
#include <SDL2/SDL.h>
#else
// Key modifiers from SDL2
typedef enum
{
	KMOD_NONE = 0x0000,
	KMOD_LSHIFT = 0x0001,
	KMOD_RSHIFT = 0x0002,
	KMOD_LCTRL = 0x0040,
	KMOD_RCTRL = 0x0080,
	KMOD_LALT = 0x0100,
	KMOD_RALT = 0x0200,
	KMOD_LGUI = 0x0400,
	KMOD_RGUI = 0x0800,
	KMOD_NUM = 0x1000,
	KMOD_CAPS = 0x2000,
	KMOD_MODE = 0x4000,
	KMOD_RESERVED = 0x8000
} SDL_Keymod;
#define KMOD_CTRL   (KMOD_LCTRL|KMOD_RCTRL)
#define KMOD_SHIFT  (KMOD_LSHIFT|KMOD_RSHIFT)
#define KMOD_ALT    (KMOD_LALT|KMOD_RALT)
#define KMOD_GUI    (KMOD_LGUI|KMOD_RGUI)
#endif

#ifdef TARGET_EMSCRIPTEN
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif


#ifdef __cplusplus
extern "C" {
#endif

#define MAX_THREAD_COUNT 128

typedef struct app_state_t app_state_t;

typedef struct mem_t {
	size_t len;
	size_t capacity;
	u8 data[0];
} mem_t;

typedef void (work_queue_callback_t)(int logical_thread_index, void* userdata);

typedef struct work_queue_entry_t {
	bool32 is_valid;
	work_queue_callback_t* callback;
	u8 userdata[128];
} work_queue_entry_t;

#if WINDOWS
typedef HANDLE semaphore_handle_t;
typedef HANDLE file_handle_t;
typedef HANDLE file_stream_t;
#else
typedef sem_t* semaphore_handle_t;
typedef int file_handle_t;
typedef FILE* file_stream_t;
#endif

typedef struct work_queue_t {
	semaphore_handle_t semaphore;
	i32 volatile next_entry_to_submit;
	i32 volatile next_entry_to_execute;
	i32 volatile completion_count;
	i32 volatile completion_goal;
	i32 volatile start_count;
	i32 volatile start_goal;
	work_queue_entry_t entries[1024];
} work_queue_t;

typedef struct benaphore_t {
	semaphore_handle_t semaphore;
	volatile i32 counter;
} benaphore_t;

typedef struct platform_thread_info_t {
	i32 logical_thread_index;
	work_queue_t* queue;
} platform_thread_info_t;

#define MAX_ASYNC_IO_EVENTS 32

typedef struct {
#if WINDOWS
	HANDLE async_io_events[MAX_ASYNC_IO_EVENTS];
	i32 async_io_index;
	OVERLAPPED overlapped;
#else
	// TODO: implement this
#endif
	u64 thread_memory_raw_size;
	u64 thread_memory_usable_size; // free space from aligned_rest_of_thread_memory onward
	void* aligned_rest_of_thread_memory;
	u32 pbo;
	arena_t temp_arena;
} thread_memory_t;

typedef struct button_state_t {
	bool8 down;
	u8 transition_count;
} button_state_t;

typedef struct analog_stick_t {
	v2f start;
	v2f end;
	bool has_input;
} analog_stick_t;

typedef struct analog_trigger_t {
	float start;
	float end;
	bool has_input;
} analog_trigger_t;

typedef struct controller_input_t {
	bool32 is_connected;
	bool32 is_analog;
	analog_stick_t left_stick;
	analog_stick_t right_stick;
	analog_trigger_t left_trigger;
	analog_trigger_t right_trigger;
	u32 modifiers;
	union {
		button_state_t buttons[533];
		struct {
			button_state_t move_up;
			button_state_t move_down;
			button_state_t move_left;
			button_state_t move_right;
			button_state_t action_up;
			button_state_t action_down;
			button_state_t action_left;
			button_state_t action_right;
			button_state_t left_shoulder;
			button_state_t right_shoulder;
			button_state_t start;
			button_state_t back;
			button_state_t button_a;
			button_state_t button_b;
			button_state_t button_x;
			button_state_t button_y;

			button_state_t keys[512];
			button_state_t key_shift;
			button_state_t key_ctrl;
			button_state_t key_alt;
			button_state_t key_super;

			// NOTE: add buttons above this line
			// cl complains about zero-sized arrays, so this the terminator is a full blown button now :(
			button_state_t terminator;
		};
	};

} controller_input_t;
// Does the count of the controller_input_t.buttons[] array add up?
STATIC_ASSERT(sizeof(((controller_input_t*)0)->buttons) == (offsetof(controller_input_t, terminator) - offsetof(controller_input_t, buttons) + sizeof(button_state_t)));

typedef struct input_t {
	button_state_t mouse_buttons[5];
	float mouse_z_start;
	float mouse_z;
	v2f drag_start_xy;
	v2f drag_vector;
	v2f mouse_xy;
	bool mouse_moved;
	float delta_t;
	union {
		controller_input_t abstract_controllers[5];
		struct {
			controller_input_t keyboard;
			controller_input_t controllers[4];
		};
	};
	u8 preferred_controller_index;
	bool are_any_buttons_down;

} input_t;

#if WINDOWS
typedef HWND window_handle_t;
#elif APPLE
typedef SDL_Window* window_handle_t;
#else
typedef SDL_Window* window_handle_t;
#endif

typedef struct {
	void* dest;
	file_handle_t file;
	i64 offset;
	size_t size_to_read;
#if WINDOWS
	OVERLAPPED overlapped;
#elif (APPLE || LINUX)
	struct aiocb cb;
#endif
} io_operation_t;

// See:
// https://github.com/SasLuca/rayfork/blob/rayfork-0.9/source/core/rayfork-core.c

enum allocator_mode {
	ALLOCATOR_MODE_UNKNOWN = 0,
	ALLOCATOR_MODE_ALLOC,
	ALLOCATOR_MODE_REALLOC,
	ALLOCATOR_MODE_FREE,
};

typedef struct allocator_t allocator_t;
struct allocator_t {
	void* userdata;
	void* (*proc)(allocator_t* this_allocator, size_t size_to_allocate, u32 mode, void* ptr_to_free_or_realloc);
};


typedef struct block_allocator_item_t block_allocator_item_t;
struct block_allocator_item_t {
	i32 chunk_index;
	i32 block_index;
	block_allocator_item_t* next;
};

typedef struct block_allocator_chunk_t {
	size_t used_blocks;
	u8* memory;
} block_allocator_chunk_t;

typedef struct block_allocator_t {
	size_t block_size;
	i32 chunk_capacity_in_blocks;
	size_t chunk_size;
	i32 chunk_count;
	i32 used_chunks;
	block_allocator_chunk_t* chunks;
	block_allocator_item_t* free_list_storage;
	block_allocator_item_t* free_list;
	i32 free_list_length;
	benaphore_t lock;
	bool is_valid;
    i32 use_count;
} block_allocator_t;

typedef struct directory_listing_t directory_listing_t;

// Inline procedures as wrappers for system routines
#if WINDOWS

static inline void semaphore_post(semaphore_handle_t semaphore) {
	ReleaseSemaphore(semaphore, 1, NULL);
}

static inline void semaphore_wait(semaphore_handle_t semaphore) {
	WaitForSingleObject(semaphore, INFINITE);
}

#else // Linux, macOS

static inline void semaphore_post(semaphore_handle_t semaphore) {
	sem_post(semaphore);
}

static inline void semaphore_wait(semaphore_handle_t semaphore) {
	sem_wait(semaphore);
}

#endif

// Platform specific function prototypes

//#if !IS_SERVER
//i64 get_clock();
//float get_seconds_elapsed(i64 start, i64 end);
//void platform_sleep(u32 ms);
//void platform_sleep_ns(i64 ns);
//i64 profiler_end_section(i64 start, const char* name, float report_threshold_ms);
//void set_swap_interval(int interval);
//u8* platform_alloc(size_t size); // required to be zeroed by the platform
//#else
//static inline u8* platform_alloc(size_t size) { return (u8*)malloc(size);}
//#endif
#define get_clock() 0
#define get_seconds_elapsed(start, end) 0

mem_t* platform_allocate_mem_buffer(size_t capacity);
mem_t* platform_read_entire_file(const char* filename);
u64 file_read_at_offset(void* dest, file_stream_t fp, u64 offset, u64 num_bytes);

void mouse_show();
void mouse_hide();
void update_cursor();
void set_cursor_default();
void set_cursor_crosshair();

const char* get_default_save_directory();
void open_file_dialog(app_state_t* app_state, u32 filetype_hint);
bool save_file_dialog(app_state_t* app_state, char* path_buffer, i32 path_buffer_size, const char* filter_string, const char* filename_hint);
void toggle_fullscreen(window_handle_t window);
bool check_fullscreen(window_handle_t window);
void set_window_title(window_handle_t window, const char* title);
void reset_window_title(window_handle_t window);
void message_box(app_state_t* app_state, const char* message);

int platform_stat(const char* filename, struct stat* st);
file_stream_t file_stream_open_for_reading(const char* filename);
file_stream_t file_stream_open_for_writing(const char* filename);
i64 file_stream_read(void* dest, size_t bytes_to_read, file_stream_t file_stream);
void file_stream_write(void* source, size_t bytes_to_write, file_stream_t file_stream);
i64 file_stream_get_filesize(file_stream_t file_stream);
i64 file_stream_get_pos(file_stream_t file_stream);
bool file_stream_set_pos(file_stream_t file_stream, i64 offset);
void file_stream_close(file_stream_t file_stream);
file_handle_t open_file_handle_for_simultaneous_access(const char* filename);
void file_handle_close(file_handle_t file_handle);
size_t file_handle_read_at_offset(void* dest, file_handle_t file_handle, u64 offset, size_t bytes_to_read);

i32 get_work_queue_task_count(work_queue_t* queue);
bool add_work_queue_entry(work_queue_t* queue, work_queue_callback_t callback, void* userdata, size_t userdata_size);
bool is_queue_work_in_progress(work_queue_t* queue);
bool is_queue_work_waiting_to_start(work_queue_t* queue);
work_queue_entry_t get_next_work_queue_entry(work_queue_t* queue);
void mark_queue_entry_completed(work_queue_t* queue);
bool do_worker_work(work_queue_t* queue, int logical_thread_index);
void drain_work_queue(work_queue_t* queue); // NOTE: only use this on the main thread
void test_multithreading_work_queue();

bool file_exists(const char* filename);
bool is_directory(const char* path);

void get_system_info(bool verbose);

benaphore_t benaphore_create(void);
void benaphore_destroy(benaphore_t* benaphore);
void benaphore_lock(benaphore_t* benaphore);
void benaphore_unlock(benaphore_t* benaphore);

void async_read_submit(io_operation_t* op);
bool async_read_has_finished(io_operation_t* op);
i64 async_read_finalize(io_operation_t* op);

block_allocator_t block_allocator_create(size_t block_size, size_t max_capacity_in_blocks, size_t chunk_size);
void block_allocator_destroy(block_allocator_t* allocator);
void* block_alloc(block_allocator_t* allocator);
void block_free(block_allocator_t* allocator, void* ptr_to_free);

void init_thread_memory(i32 logical_thread_index);

unsigned int crc32(unsigned char* buffer, int len);
unsigned int crc32_skip_carriage_return(unsigned char* buffer, int len);


//#if IS_SERVER
//#define console_print printf
//#define console_print_error(...) fprintf(stderr, __VA_ARGS__)
//#define console_print_verbose(...) do { if (is_verbose_mode) fprintf(stdout, __VA_ARGS__); } while(0)
//#else
//void console_print(const char* fmt, ...); // defined in console.cpp
//void console_print_verbose(const char* fmt, ...); // defined in console.cpp
//void console_print_error(const char* fmt, ...); // // defined in console.cpp
//#endif
#if IS_DEBUG
#define console_print(msg, ...) printf("%s:%d: " msg "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#define console_print_error(msg, ...) printf("ERROR %s:%d: " msg "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
#define console_print(msg, ...)
#define console_print_error(msg, ...)
#endif

#if IS_DEBUG_VERBOSE
#define console_print_verbose(msg, ...) printf("VERBOSE %s:%d: " msg "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)
#else
#define console_print_verbose(msg, ...)
#endif


// globals
#if defined(PLATFORM_IMPL)
#define INIT(...) __VA_ARGS__
#define extern
#else
#define INIT(...)
#undef extern
#endif

// TODO(avirodov): I disabled thread-local memory allocation here, need to either re-enable it if we want to support
//  thread-specific pools, and initialize it properly on new threads, or have a more proper solution.
extern /*THREAD_LOCAL*/ thread_memory_t* local_thread_memory;
static inline temp_memory_t begin_temp_memory_on_local_thread() { return begin_temp_memory(&local_thread_memory->temp_arena); }

extern int g_argc;
extern const char** g_argv;
extern bool is_fullscreen;
extern bool is_program_running;
extern bool need_quit;
extern input_t inputs[2];
extern input_t *old_input;
extern input_t *curr_input;
extern u32 os_page_size;
extern u64 page_alignment_mask;
extern i32 total_thread_count;
extern i32 worker_thread_count;
extern i32 physical_cpu_count;
extern i32 logical_cpu_count;
extern bool is_vsync_enabled;
extern bool is_nvidia_gpu;
extern bool is_macos;
extern work_queue_t global_work_queue;
extern work_queue_t global_completion_queue;
extern work_queue_t global_export_completion_queue;
extern i32 global_worker_thread_idle_count;
extern THREAD_LOCAL i32 work_queue_call_depth;
extern bool is_verbose_mode INIT(= false);
extern benaphore_t console_printer_benaphore;
extern bool cursor_hidden;
extern const char* global_settings_dir;
extern char global_export_save_as_filename[512];
extern bool save_file_dialog_open;

extern float total_rgb_transform_time;

extern bool is_dicom_available;
extern bool is_dicom_loading_done;


#undef INIT
#undef extern

#ifdef __cplusplus
}
#endif

