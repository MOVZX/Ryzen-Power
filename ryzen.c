#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define RAPL_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define USEC 1000000

static int64_t last_read_time = 0;
static int64_t cached_consumption = -1;

int64_t get_monotonicTimeUSec()
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);

    return time.tv_sec * USEC + time.tv_nsec / 1000;
}

int64_t get_cpuConsumptionUJoules()
{
    int64_t current_time = get_monotonicTimeUSec();

    if (current_time - last_read_time >= USEC)
    {
        FILE *file = fopen(RAPL_PATH, "r");

        if (file != NULL)
        {
            if (fscanf(file, "%lld", &cached_consumption) != 1)
                cached_consumption = -1;

            fclose(file);
        }
        else
            perror("Failed to open RAPL path");

        last_read_time = current_time;
    }

    return cached_consumption;
}

float get_cpuConsumptionWatts()
{
    static int64_t previous_usage = -1;
    static int64_t previous_timestamp = 0;

    int64_t current_timestamp = get_monotonicTimeUSec();
    int64_t current_usage = get_cpuConsumptionUJoules();

    if (current_usage < 0 || previous_timestamp == current_timestamp)
        return 0;

    if (previous_usage < 0)
    {
        previous_usage = current_usage;
        previous_timestamp = current_timestamp;

        usleep(USEC);

        current_timestamp = get_monotonicTimeUSec();
        current_usage = get_cpuConsumptionUJoules();
    }

    float watts = (float)(current_usage - previous_usage) / (float)(current_timestamp - previous_timestamp);
    previous_timestamp = current_timestamp;
    previous_usage = current_usage;

    return watts;
}

int main()
{
    printf("%.2f\n", get_cpuConsumptionWatts());

    return 0;
}
