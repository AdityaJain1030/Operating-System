#include "syscall.h"
#include "timer.h"



void date() {
    int t = rd_time();


    

    int days = t/ 86400;
    int sod = t % 86400;
    int hour = sod % 60;
    int minute = (sod % 3600) / 60;
    int second = sod % 60;

    
    int year = 1970; //FIXME: FIND YEAR?????

    int leap = (int)((year % 4) == 0);

    int month_lengths[12] = {31, 28+leap, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    //went with static const below so snprintf doesn't crashout
    static const char * month_names[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    while (1){
        int diy = 365+leap;

        if (days >= diy){
            int days -= diy;
            int year ++;
        }else break;
    }


    int month = 0;
    //for (ml in month_lengths:
    for (int i = 0; i < 12; i++){
        if (days >= month_lengths[i]){
            days -= i;
            month += 1;
        }else break;
    }

    int day = days + 1;

    

    // Print the date to STDOUT
    
    //format 05 Dec 2025 18:00:00 -> (has a constant 20 characters not including the null terminator)
    char buf[20];

    snprintf(buf, 20, "%d%d %s %d %d%d:%d%d:%d%d",
            day/10, day%10,
            month_names[month],
            year,
            hour/10, hour%10,
            minute/10, minute%10,
            second/10, second%10
            );


    _write(STDOUT, buf, strlen(buf));
    _write(STDOUT, "\r\n", 2);
    
    

    return;
}

void main(int argc, char *argv[]){

    date();
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
