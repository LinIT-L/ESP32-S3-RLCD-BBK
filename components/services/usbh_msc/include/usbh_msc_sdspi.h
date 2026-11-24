#ifndef USBH_MSC_SDSPI_H
#define USBH_MSC_SDSPI_H

#include "esp_err.h"
#include <stdbool.h>

esp_err_t usbh_msc_start(void);
void usbh_msc_stop(void);
bool usbh_msc_is_running(void);

#endif
