#pragma once

#include <stdint.h>
#include <stddef.h>

#define LOG_DATA_VERSION 1

#define LOG_DATA_TYPE_TIME      0
#define LOG_DATA_TYPE_ITS       1
#define LOG_DATA_TYPE_GPS       2
#define LOG_DATA_TYPE_INFO    254
#define LOG_DATA_TYPE_VERSION 255

#define MAX_PKT_SIZE 2000
#pragma pack(push, 1)

struct Its
{
  uint8_t payload[MAX_PKT_SIZE] ;
} ;

struct Gps
{
  int32_t  latitude;    // °, * 10^7
  int32_t  longitude;   // °, * 10^7
  int32_t  altitudeWGS; // m, * 10^3
  int32_t  altitudeMSL; // m, * 10^3
  int32_t  speed;       // m/s, * 10^2
  int32_t  heading;     // °, * 10^5
} ;

struct Info
{
  char    text[500];
} ;

struct Version
{
  uint8_t logVersion;   // 1
  uint8_t prgVerMaj;
  uint8_t prgVerMin;
  uint8_t prgVerRev;
} ;

struct Header
{
  uint8_t  pkt_type;     // log_data_type_t
  uint8_t  reserved;     // 0
  uint16_t body_size;    // body size  (Time 0, ITS 1..MAX_PKT_SIZE, GPS 24, Version 4, Info strlen)
  int64_t  timestamp_us; // UTC time in µs after GPS fix, else ESP system time in µs
} ;

union Body
{
  struct Its     its;
  struct Gps     gps;
  struct Info    info;
  struct Version version;
} ;

struct LogData
{
  struct Header header ;
  union Body body ;
} ;

#pragma pack(pop)

// Compile-time checks of the binary log layout (USB stream and SD files).
// Any change here is a log format change: bump LOG_DATA_VERSION and update
// the README, the bridge (esp32-c5-its-otm) and log-cvt (esp32-c5-its-log).
#if defined(__cplusplus)
#define LOG_DATA_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define LOG_DATA_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

LOG_DATA_ASSERT(sizeof(struct Header) == 12, "log format: struct Header must be 12 bytes") ;
LOG_DATA_ASSERT(offsetof(struct Header, body_size) == 2, "log format: Header.body_size must be at offset 2") ;
LOG_DATA_ASSERT(offsetof(struct Header, timestamp_us) == 4, "log format: Header.timestamp_us must be at offset 4") ;
LOG_DATA_ASSERT(sizeof(struct Gps) == 24, "log format: struct Gps must be 24 bytes") ;
LOG_DATA_ASSERT(sizeof(struct Version) == 4, "log format: struct Version must be 4 bytes") ;
LOG_DATA_ASSERT(MAX_PKT_SIZE <= UINT16_MAX, "log format: MAX_PKT_SIZE must fit in uint16_t body_size") ;
