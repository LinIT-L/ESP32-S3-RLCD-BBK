#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t lavaxvm_vm_ram_size(void);
size_t lavaxvm_local_ram_size(void);
bool lavaxvm_init(void *vm_ram, size_t vm_ram_size,
                  void *local_ram, size_t local_ram_size);
bool lavaxvm_load_shell(void);
void lavaxvm_run(void);
void lavaxvm_shutdown(void);
const char *lavaxvm_last_error(void);

void lavax_platform_bind_display(void *surface0, void *surface1);
void lavax_platform_set_app_path(const char *path);
void lavax_platform_redraw(void);
void lavax_platform_present_indexed(const uint8_t *pixels, int width, int height, int stride,
                                    const uint8_t *palette, int graph_mode,
                                    int canvas_width, int canvas_height, int display_scale,
                                    int mask_enabled);
void lavax_platform_present_rgb555(const uint16_t *pixels, int width, int height, int stride,
                                   int canvas_width, int canvas_height, int display_scale,
                                   int mask_enabled);
void lavax_platform_poll(void);
int lavax_platform_should_exit(void);
int lavax_platform_take_app_exit_request(void);
int lavax_platform_app_exit_combo_down(void);
const char *lavax_host_path(const char *path, char *buffer, size_t size);
