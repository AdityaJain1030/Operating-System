#include "syscall.h"
#include "string.h"
#include <stdint.h>
#include "shell.h"

#define NS2SEC 1000000000

void date(uint64_t t) {
    uint64_t days = t / 86400;
    uint64_t sod = t % 86400;
    uint64_t hour = sod / 3600;
    uint64_t minute = (sod % 3600) / 60;
    uint64_t second = sod % 60;

    
    int year = 1970; //FIXME: FIND YEAR?????

    // maybe this 
    // int leap = (int)(year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    int leap = 0;

    int month_lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    //went with static const below so snprintf doesn't crashout
    static const char * month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    while (1){
        leap = (int)((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
        int diy = 365+leap;

        if (days >= diy){
            days -= diy;
            year ++;
        }else break;
    }

    month_lengths[1] = 28 + leap;

    int month = 0;
    //for (ml in month_lengths:
    for (int i = 0; i < 12; i++){
        if (days >= month_lengths[i]){
            days -= month_lengths[i];
            month += 1;
        }else break;
    }

    int day = days + 1;
    // Print the date to STDOUT
    
    //format 05 Dec 2025 18:00:00 -> (has a constant 20 characters not including the null terminator)
    printf("%d%d %s %d %d%d:%d%d:%d%d\r",
            day/10, day%10,
            month_names[month],
            year,
            hour/10, hour%10,
            minute/10, minute%10,
            second/10, second%10
            );

    // printf("%s\r\n", buf);
    // _write(STDOUT, buf, strlen(buf));
    // _write(STDOUT, "\r\n", 2);
    return;
}

void main(int argc, char **argv){
    
    int fd = _open(-1, "dev/rtc0");
    if (fd < 0)
    {
        printf("Date: cannot open dev/rtc0 \r\n");
        _exit();
    }

    // making sure type matches with rtc.c
    uint64_t time_ns;
    int err = _read(fd, &time_ns, sizeof(uint64_t));
    if (err < 0) {
        printf("Could not get time from dev/rtc0 \r\n");
        _exit();
    }
    // printf("%d", time_ns);
    date((uint64_t)(time_ns / NS2SEC));
    // printf("[es]");
    _exit();
    
}

/*
function fromUnix(t):
    # 1) split into days and secs
    days = t // 86400
    sod  = t % 86400
    hour = sod // 3600
    minute = (sod % 3600) // 60
    second = sod % 60

    # 2) find year
    year = 1970
    while True:
        diy = 366 if leap(year) else 365
        if days >= diy:
            days -= diy
            year += 1
        else:
            break

    # 3) find month
    month_lengths = [31, 28 + leap(year), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    month = 1
    for ml in month_lengths:
        if days >= ml:
            days -= ml
            month += 1
        else:
            break

    day = days + 1

    return (year, month, day, hour, minute, second)

 */