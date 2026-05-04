#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/random.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/ioctl.h>
#include <linux/time.h>
#include <linux/string.h>

#define DEVICE_NAME "iot_sensor"
#define LOG_DEVICE_NAME "iot_log"
#define MAX_SIZE 64
#define LOG_SIZE 1024
#define MAX_LOG_ENTRIES 50

#define IOC_MAGIC 'k'
#define IOC_SET_SENSOR _IOW(IOC_MAGIC, 1, int)
#define IOC_SET_UNITS _IOW(IOC_MAGIC, 2, int)
#define IOC_SET_RANGE _IOW(IOC_MAGIC, 3, struct my_range)

enum sensor_type { TEMP, HUMIDITY, PRESSURE };
enum temp_unit { CELSIUS, FAHRENHEIT };

struct my_range {
    int min;
    int max;
};

struct log_entry {
    char data[MAX_SIZE];
    ktime_t timestamp;
};

struct iot_device {
    enum sensor_type type;
    enum temp_unit unit;
    struct my_range range;
    struct semaphore sem;
    struct log_entry logs[MAX_LOG_ENTRIES];
    int log_index;
    struct cdev sensor_cdev;
    struct cdev log_cdev;
};

static struct iot_device device;
static dev_t sensor_dev_number;
static dev_t log_dev_number;
static struct class *sensor_class;
static struct class *log_class;

static int device_open(struct inode *inode, struct file *file) {
    if (down_interruptible(&device.sem)) {
        return -ERESTARTSYS;
    }

    printk(KERN_INFO "iot_sensor: Device opened\n");
    return 0;
}

static int device_release(struct inode *inode, struct file *file) {
    up(&device.sem);
    printk(KERN_INFO "iot_sensor: Device released\n");
    return 0;
}

static ssize_t device_read(struct file *file, char __user *buffer, size_t length, loff_t *offset) {
    char data[MAX_SIZE];
    int value;
    unsigned int rand_val;
    size_t bytes_to_copy;

    if (*offset > 0) {
        return 0;
    }

    get_random_bytes(&rand_val, sizeof(rand_val));

    if ((rand_val % 100) < 5) {
        printk(KERN_WARNING "iot_sensor: Simulated sensor failure\n");
        return -EIO;
    }

    get_random_bytes(&rand_val, sizeof(rand_val));
    value = device.range.min + (rand_val % (device.range.max - device.range.min + 1));

    switch (device.type) {
        case TEMP:
            snprintf(data, MAX_SIZE, "Temperature: %d %c\n", value,
                     device.unit == CELSIUS ? 'C' : 'F');
            break;

        case HUMIDITY:
            snprintf(data, MAX_SIZE, "Humidity: %d%%\n", value);
            break;

        case PRESSURE:
            snprintf(data, MAX_SIZE, "Pressure: %d hPa\n", value);
            break;

        default:
            snprintf(data, MAX_SIZE, "Unknown sensor\n");
            break;
    }

    strncpy(device.logs[device.log_index].data, data, MAX_SIZE - 1);
    device.logs[device.log_index].data[MAX_SIZE - 1] = '\0';
    device.logs[device.log_index].timestamp = ktime_get();
    device.log_index = (device.log_index + 1) % MAX_LOG_ENTRIES;

    bytes_to_copy = min(length, strlen(data));

    if (copy_to_user(buffer, data, bytes_to_copy)) {
        return -EFAULT;
    }

    *offset += bytes_to_copy;
    return bytes_to_copy;
}

static ssize_t device_write(struct file *file, const char __user *buffer, size_t length, loff_t *offset) {
    char data[MAX_SIZE];
    size_t bytes_to_copy;

    bytes_to_copy = min(length, (size_t)(MAX_SIZE - 1));

    if (copy_from_user(data, buffer, bytes_to_copy)) {
        return -EFAULT;
    }

    data[bytes_to_copy] = '\0';

    printk(KERN_INFO "iot_sensor: Written value: %s\n", data);

    return bytes_to_copy;
}

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    int sensor;
    struct my_range new_range;

    switch (cmd) {
        case IOC_SET_SENSOR:
            if (copy_from_user(&sensor, (int __user *)arg, sizeof(int))) {
                return -EFAULT;
            }

            if (sensor < 0 || sensor > 2) {
                return -EINVAL;
            }

            device.type = sensor;
            break;

        case IOC_SET_UNITS:
            if (copy_from_user(&sensor, (int __user *)arg, sizeof(int))) {
                return -EFAULT;
            }

            if (device.type == TEMP && sensor >= 0 && sensor <= 1) {
                device.unit = sensor;
            } else {
                return -EINVAL;
            }

            break;

        case IOC_SET_RANGE:
            if (copy_from_user(&new_range, (struct my_range __user *)arg, sizeof(struct my_range))) {
                return -EFAULT;
            }

            if (new_range.min < new_range.max) {
                device.range = new_range;
            } else {
                return -EINVAL;
            }

            break;

        default:
            return -ENOTTY;
    }

    return 0;
}

static ssize_t log_read(struct file *file, char __user *buffer, size_t length, loff_t *offset) {
    char log_data[LOG_SIZE];
    int len = 0;
    int i;
    size_t bytes_to_copy;

    if (*offset > 0) {
        return 0;
    }

    memset(log_data, 0, LOG_SIZE);

    for (i = 0; i < MAX_LOG_ENTRIES; i++) {
        if (strlen(device.logs[i].data) > 0 && len < LOG_SIZE - MAX_SIZE) {
            len += snprintf(log_data + len, LOG_SIZE - len, "[%lld] %s",
                            ktime_to_ms(device.logs[i].timestamp),
                            device.logs[i].data);
        }
    }

    bytes_to_copy = min(length, (size_t)len);

    if (copy_to_user(buffer, log_data, bytes_to_copy)) {
        return -EFAULT;
    }

    *offset += bytes_to_copy;
    return bytes_to_copy;
}

static struct file_operations sensor_fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .write = device_write,
    .unlocked_ioctl = device_ioctl,
};

static struct file_operations log_fops = {
    .owner = THIS_MODULE,
    .read = log_read,
};

static int __init iot_sensor_init(void) {
    int ret;

    sema_init(&device.sem, 1);
    device.type = TEMP;
    device.unit = CELSIUS;
    device.range.min = 0;
    device.range.max = 50;
    device.log_index = 0;

    ret = alloc_chrdev_region(&sensor_dev_number, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "iot_sensor: Failed to allocate sensor major number\n");
        return ret;
    }

    cdev_init(&device.sensor_cdev, &sensor_fops);
    ret = cdev_add(&device.sensor_cdev, sensor_dev_number, 1);
    if (ret < 0) {
        unregister_chrdev_region(sensor_dev_number, 1);
        return ret;
    }

    sensor_class = class_create(DEVICE_NAME);
    if (IS_ERR(sensor_class)) {
        cdev_del(&device.sensor_cdev);
        unregister_chrdev_region(sensor_dev_number, 1);
        return PTR_ERR(sensor_class);
    }

    if (IS_ERR(device_create(sensor_class, NULL, sensor_dev_number, NULL, DEVICE_NAME))) {
        class_destroy(sensor_class);
        cdev_del(&device.sensor_cdev);
        unregister_chrdev_region(sensor_dev_number, 1);
        return -1;
    }

    ret = alloc_chrdev_region(&log_dev_number, 0, 1, LOG_DEVICE_NAME);
    if (ret < 0) {
        device_destroy(sensor_class, sensor_dev_number);
        class_destroy(sensor_class);
        cdev_del(&device.sensor_cdev);
        unregister_chrdev_region(sensor_dev_number, 1);
        return ret;
    }

    cdev_init(&device.log_cdev, &log_fops);
    ret = cdev_add(&device.log_cdev, log_dev_number, 1);
    if (ret < 0) {
        unregister_chrdev_region(log_dev_number, 1);
        device_destroy(sensor_class, sensor_dev_number);
        class_destroy(sensor_class);
        cdev_del(&device.sensor_cdev);
        unregister_chrdev_region(sensor_dev_number, 1);
        return ret;
    }

    log_class = class_create(LOG_DEVICE_NAME);
    if (IS_ERR(log_class)) {
        cdev_del(&device.log_cdev);
        unregister_chrdev_region(log_dev_number, 1);
        device_destroy(sensor_class, sensor_dev_number);
        class_destroy(sensor_class);
        cdev_del(&device.sensor_cdev);
        unregister_chrdev_region(sensor_dev_number, 1);
        return PTR_ERR(log_class);
    }

    if (IS_ERR(device_create(log_class, NULL, log_dev_number, NULL, LOG_DEVICE_NAME))) {
        class_destroy(log_class);
        cdev_del(&device.log_cdev);
        unregister_chrdev_region(log_dev_number, 1);
        device_destroy(sensor_class, sensor_dev_number);
        class_destroy(sensor_class);
        cdev_del(&device.sensor_cdev);
        unregister_chrdev_region(sensor_dev_number, 1);
        return -1;
    }

    printk(KERN_INFO "iot_sensor: Initialized. Sensor Major: %d, Log Major: %d\n",
           MAJOR(sensor_dev_number), MAJOR(log_dev_number));

    return 0;
}

static void __exit iot_sensor_exit(void) {
    device_destroy(log_class, log_dev_number);
    class_destroy(log_class);
    cdev_del(&device.log_cdev);
    unregister_chrdev_region(log_dev_number, 1);

    device_destroy(sensor_class, sensor_dev_number);
    class_destroy(sensor_class);
    cdev_del(&device.sensor_cdev);
    unregister_chrdev_region(sensor_dev_number, 1);

    printk(KERN_INFO "iot_sensor: Unloaded\n");
}

module_init(iot_sensor_init);
module_exit(iot_sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("I.B. Kamara");
MODULE_DESCRIPTION("Advanced Virtual IoT Multi-Sensor Driver");