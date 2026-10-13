#include <iostream>
#include <cstring>
#include <chrono>

#include "log-cvt.h"


// The log time is ESP uptime until a GPS fix, then UTC.
static const uint64_t utcStartUs = 1000000000000000 ; // 2001-09-09, uptime never gets there
static const uint64_t utcEndUs   = 4102444800000000 ; // 2100-01-01, later log times are corrupt

void BlfWriter::timeReset()
{
  _timeFirst = true ;
  _timeUtc = false ;
  _timeMeasurementStartUs = 0 ;
  _timeOffsetUs = 0 ;
  _timeLastUs = 0 ;
}

void BlfWriter::utcTimeToBlfTime(uint64_t utcTimeS, Vector::BLF::SYSTEMTIME &blfTime)
{
  // no gmtime(): time_t is 32 bit on some hosts, dates after 2038 would wrap
  using namespace std::chrono ;
  sys_seconds t{seconds{utcTimeS}} ;
  sys_days d = floor<days>(t) ;
  year_month_day ymd{d} ;
  hh_mm_ss<seconds> hms{t - d} ;

  blfTime.year = (int)ymd.year() ;
  blfTime.month = (unsigned)ymd.month() ;
  blfTime.day = (unsigned)ymd.day() ;
  blfTime.dayOfWeek = weekday{d}.c_encoding() ;
  blfTime.hour = hms.hours().count();
  blfTime.minute = hms.minutes().count();
  blfTime.second = hms.seconds().count();
  blfTime.milliseconds = 0;
}

// BLF object times never step back. UTC (after a GPS fix) is used as is.
// After a device restart (uptime steps back or follows UTC) uptime continues
// from the latest object time. No offset is carried from one time base to
// the other.
uint64_t BlfWriter::timeNs(uint64_t timeUs)
{
  if (timeUs >= utcEndUs) // e.g. a record cut by a device restart
  {
    std::cerr << "corrupt log time " << timeUs << " us, using the latest object time\n" ;
    return (_timeLastUs - _timeMeasurementStartUs) * 1000 ;
  }

  uint64_t timeS = timeUs / 1000000 ;
  bool utc = timeUs >= utcStartUs ;

  if (_timeFirst)
  {
    _timeMeasurementStartUs = timeS * 1000000 ;
    _timeLastUs = timeUs ;

    utcTimeToBlfTime(timeS, _blf.fileStatistics.measurementStartTime) ;

    _timeFirst = false ;
  }
  else if (utc) // GPS fix
  {
    _timeOffsetUs = 0 ;
  }
  else if (_timeUtc || timeUs + _timeOffsetUs + 1000000 < _timeLastUs) // device restart
  {
    _timeOffsetUs = _timeLastUs ;
    std::cerr
      << "log time steps back to " << timeUs
      << " us (device restart?), blf time continues from the latest object\n" ;
  }
  _timeUtc = utc ;

  uint64_t blfUs = timeUs + _timeOffsetUs ;
  if (blfUs < _timeLastUs) // small step back, e.g. GPS and ITS records logged out of order
    blfUs = _timeLastUs ;
  _timeLastUs = blfUs ;

  utcTimeToBlfTime(blfUs / 1000000, _blf.fileStatistics.lastObjectTime) ;

  return (blfUs - _timeMeasurementStartUs) * 1000 ;
}

BlfWriter::BlfWriter()
{
}
BlfWriter::~BlfWriter()
{
}

bool BlfWriter::open(const std::string &logName)
{
  timeReset() ;
  std::string blfName = logName + ".blf" ;
  _blf.open(blfName.c_str(), std::ios_base::out) ;
  if (!_blf.is_open())
  {
    std::cerr << "open blf " << blfName << " failed\n" ;
    return false ;
  }
  return true ;
}

bool BlfWriter::close()
{
  _blf.close() ;
  return true ;
}

bool BlfWriter::writeIts(uint64_t timeUs, const std::vector<uint8_t> &data0)
{
  std::vector<uint8_t> data = data0 ;

  if (data.size() > 32) // add pseudo eth layer
    data.insert(data.begin() + 32, data0.begin() + 4, data0.begin() + 4 + 12) ;

  auto *wlanFrame = new Vector::BLF::WlanFrame ;
  wlanFrame->objectTimeStamp = timeNs(timeUs);

  wlanFrame->channel = 1 ;
  wlanFrame->flags = 7 ;
  wlanFrame->dir = Vector::BLF::WlanFrame::Dir::Rx ;
  wlanFrame->radioChannel = 180 ;
  wlanFrame->frameLength = data.size() ;
  wlanFrame->frameData = data ;

  _blf.write(wlanFrame) ;
  return true ;
}

bool BlfWriter::writeGps(uint64_t timeUs, const std::vector<uint8_t> &data)
{
  const static std::string gnssSysVarName = "::GNSS::Ath1" ;
#pragma pack(push, 1)
  struct GnssSysVarData
  {
    double latitude ;
    double longitude ;
    double altitude ;
    double speed ;
    double direction ;
    uint32_t mode ;
    uint32_t status ;
    uint32_t satInView ;
    uint32_t satInUse ;
    double gnssTime ;
    double hdop ;
    double pdop ;
    double vdop ;
    double magVar ;
    double geoAlt ;
  } ;
  GnssSysVarData gnssSysVarData ;
#pragma pack(pop)

  if (data.size() < sizeof(Gps))
    return false ;
  Gps gps ;
  memcpy(&gps, data.data(), sizeof(gps)) ;

  memset(&gnssSysVarData, 0, sizeof(gnssSysVarData)) ;

  gnssSysVarData.latitude = (double)gps.latitude / 1e7 ;
  gnssSysVarData.longitude = (double)gps.longitude / 1e7;
  gnssSysVarData.altitude = (double)gps.altitudeMSL / 1e3;
  gnssSysVarData.speed = (double)gps.speed / 1e2 ;
  gnssSysVarData.direction = (double)gps.heading / 1e5 ;
  gnssSysVarData.mode = 3 ;
  gnssSysVarData.status = 0 ;
  gnssSysVarData.satInView = 10 ;
  gnssSysVarData.satInUse = 10 ;
  gnssSysVarData.gnssTime = (double)timeUs / 1e6 ;
  gnssSysVarData.geoAlt = (double)gps.altitudeWGS / 1e3 ;

  auto gnssSysVar = new Vector::BLF::SystemVariable ;
  gnssSysVar->objectTimeStamp = timeNs(timeUs) ;

  gnssSysVar->type = Vector::BLF::SystemVariable::ByteArray ;
  gnssSysVar->nameLength = gnssSysVarName.size() ;
  gnssSysVar->dataLength = sizeof(GnssSysVarData) ;
  gnssSysVar->name = gnssSysVarName ;
  gnssSysVar->data.assign((uint8_t*)&gnssSysVarData, (uint8_t*)&gnssSysVarData + sizeof(GnssSysVarData)) ;

  _blf.write(gnssSysVar) ;
  return true ;
}
