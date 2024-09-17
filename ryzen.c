#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // For usleep function
#include <sys/time.h> // For gettimeofday function
#include <stdint.h> // For fixed-width integer types

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"

#define USEC 1000000

// Function to get the current energy consumption in microjoules
int64_t get_cpuConsumptionUJoules()
{
    int64_t consumption = -1;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (file == NULL)
    {
        perror("Error opening RAPL energy file");

        return -1;
    }

    if (fscanf(file, "%lld", &consumption) != 1)
    {
        perror("Error reading energy consumption");

        consumption = -1;
    }

    fclose(file);

    return consumption;
}

// Function to get the current time in microseconds
int64_t get_currentTimeUSec()
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("Error getting current time");

        return -1;
    }

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

int main()
{
    int64_t initial_usage = get_cpuConsumptionUJoules();
    int64_t initial_time = get_currentTimeUSec();

    if (initial_usage == -1 || initial_time == -1)
    {
        fprintf(stderr, "Failed to read initial CPU consumption or time data.\n");

        return 1;
    }

    // Short delay to allow measurable change
    usleep(USEC); // Sleep for 1 second

    int64_t final_usage = get_cpuConsumptionUJoules();
    int64_t final_time = get_currentTimeUSec();

    if (final_usage == -1 || final_time == -1)
    {
        fprintf(stderr, "Failed to read final CPU consumption or time data.\n");

        return 1;
    }

    // Calculate the time difference and energy difference
    if (final_time < initial_time)
    {
        fprintf(stderr, "Time went backwards.\n");

        return 1;
    }

    int64_t time_diff_usec = final_time - initial_time;
    int64_t energy_diff_uj = final_usage - initial_usage;

    if (time_diff_usec <= 0)
    {
        fprintf(stderr, "Invalid time difference.\n");

        return 1;
    }

    if (energy_diff_uj < 0)
    {
        fprintf(stderr, "Energy consumption decreased.\n");

        return 1;
    }

    // Convert time difference to seconds
    double time_diff_sec = time_diff_usec / USEC;

    // Calculate power in watts
    double watts = energy_diff_uj / (time_diff_sec * USEC);

    // Print the result
    printf("%.2lf\n", watts);

    return 0;
}
