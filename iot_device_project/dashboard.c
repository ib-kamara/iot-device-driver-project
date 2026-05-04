#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>

#define DEVICE_PATH "/dev/iot_sensor"
#define LOG_PATH "/dev/iot_log"

#define IOC_MAGIC 'k'
#define IOC_SET_SENSOR _IOW(IOC_MAGIC, 1, int)
#define IOC_SET_UNITS _IOW(IOC_MAGIC, 2, int)

int main() {
    int fd = open(DEVICE_PATH, O_RDWR);
    int log_fd = open(LOG_PATH, O_RDONLY);

    char buffer[64];

    int sensor = 0; // TEMP
    int unit = 0;   // CELSIUS

    ioctl(fd, IOC_SET_SENSOR, &sensor);
    ioctl(fd, IOC_SET_UNITS, &unit);

    printf("=== IoT Sensor Dashboard ===\n");

    read(fd, buffer, sizeof(buffer));
    printf("Sensor Reading: %s\n", buffer);

    printf("\nRecent Logs:\n");
    read(log_fd, buffer, sizeof(buffer));
    printf("%s\n", buffer);

    close(fd);
    close(log_fd);

    return 0;
}