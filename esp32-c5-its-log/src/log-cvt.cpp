#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>

#include <iostream>
#include <vector>
#include <sys/types.h>

#include "log-cvt.h"

// with a 32 bit off_t, fopen() fails on logs over 2 GiB
static_assert(sizeof(off_t) == 8, "build with -D_FILE_OFFSET_BITS=64") ;


Writer::Writer() {}
Writer::~Writer() {}

bool Writer::open(const std::string &) { return true ; }
bool Writer::close() { return true ; }
bool Writer::writeIts(uint64_t, const std::vector<uint8_t>&) { return true ; }
bool Writer::writeGps(uint64_t, const std::vector<uint8_t>&) { return true ; }

template <class t>
bool read(FILE *log, t &v)
{
  if (fread(&v, sizeof(t), 1, log) != 1)
  {
    return false ;
  }
  return true ;
}

bool read(FILE *log, size_t size, std::vector<uint8_t> &v)
{
  v.resize(size) ;
  if (fread(v.data(), sizeof(uint8_t), v.size(), log) != v.size())
  {
    return false ;
  }
  return true ;
}

bool skip_magic(FILE *log)
{
  uint8_t c ;
  uint32_t magic{0} ;

  while (read(log, c))
  {
    magic <<= 8 ;
    magic |= c ;
    if (magic == 0xaa5555aa)
      return true ;
  }
  return false ;
}


// check a header against the log format log-cvt was built with
bool header_valid(const Header &header)
{
  if ((header.reserved != 0) ||
      (header.timestamp_us < 0) ||
      (header.body_size > sizeof(Body)))
    return false ;

  switch (header.pkt_type)
  {
  case LOG_DATA_TYPE_TIME:
    return header.body_size == 0 ;
  case LOG_DATA_TYPE_ITS:
    return header.body_size > 0 ;
  case LOG_DATA_TYPE_GPS:
    return header.body_size == sizeof(Gps) ;
  case LOG_DATA_TYPE_VERSION:
    return header.body_size == sizeof(Version) ;
  default: // info and unknown types
    return true ;
  }
}

// usb log: a record must be followed by a magic or the end of the file.
// If a magic with a valid header starts inside the record instead, the
// receiver was reset while sending it: drop the record, resync there.
bool record_complete(FILE *log, off_t start, const Header &header, const std::vector<uint8_t> &body)
{
  auto is_magic = [](const uint8_t *p)
  {
    return (p[0] == 0xaa) && (p[1] == 0x55) && (p[2] == 0x55) && (p[3] == 0xaa) ;
  } ;
  uint8_t next[4 + sizeof(Header) - 1] ; // a magic and header may start in the last body byte
  off_t end = ftello(log) ;
  if (end < 0) // not seekable, e.g. a pipe: cannot look ahead
    return true ;
  size_t n = fread(next, 1, sizeof(next), log) ;

  fseeko(log, end, SEEK_SET) ;
  if ((n < 4) || is_magic(next)) // end of file or next record
    return true ;

  std::vector<uint8_t> data((const uint8_t*)&header, (const uint8_t*)&header + sizeof(Header)) ;
  data.insert(data.end(), body.begin(), body.end()) ;
  data.insert(data.end(), next, next + n) ;

  for (size_t pos = 0 ; (pos < sizeof(Header) + body.size()) && (pos + 4 + sizeof(Header) <= data.size()) ; ++pos)
  {
    if (is_magic(&data[pos]) && header_valid(*(const Header*)&data[pos + 4]))
    {
      std::cerr << "record at offset " << start << " cut short, dropped\n" ;
      fseeko(log, start + 4 + (off_t) pos, SEEK_SET) ;
      return false ;
    }
  }

  std::cerr << "record at offset " << start << " followed by non-log data\n" ;
  return true ;
}

bool log_cvt(FILE *log, bool isUsbLog, std::unique_ptr<Writer> &writer, ContentCount &count)
{
  Header header ;
  std::vector<uint8_t> body ;
  off_t start = ftello(log) - (isUsbLog ? 4 : 0) ; // record offset, usb: of its magic

  if (!read(log, header))
    return false ;

  if (!header_valid(header))
  {
    std::cerr << "invalid record header at offset " << start << "\n" ;
    if (!isUsbLog)
      return false ;
    // not a record (e.g. boot text after a magic): rescan from the byte
    // after this magic, the next magic may overlap it
    fseeko(log, start + 1, SEEK_SET) ;
    return true ;
  }

  if (!read(log, header.body_size, body))
  {
    if (!isUsbLog || ferror(log))
      return false ;
    // the body runs past the end of the file, e.g. the receiver was reset
    // while sending it: rescan behind the header for later records
    std::cerr << "record at offset " << start << " truncated\n" ;
    fseeko(log, start + 4 + (off_t) sizeof(Header), SEEK_SET) ;
    return true ;
  }

  if (isUsbLog && !record_complete(log, start, header, body))
    return true ;

  switch (header.pkt_type)
  {
  case LOG_DATA_TYPE_TIME: // ignore
    count.time += 1 ;
    break ;
  case LOG_DATA_TYPE_ITS:
    if (writer->writeIts(header.timestamp_us, body))
      count.its += 1 ;
    break ;
  case LOG_DATA_TYPE_GPS:
    if (writer->writeGps(header.timestamp_us, body))
      count.gps += 1 ;
    break ;
  case LOG_DATA_TYPE_INFO:
    count.info += 1 ;
    std::cout
      << "\nInfo:\n"
      << std::string((char*)body.data(), body.size()) ;
    break ;
  case LOG_DATA_TYPE_VERSION:
    count.version += 1 ;
    {
      if (body.size() < sizeof(Version))
        break ;
      const Version &version = *(Version*)body.data() ;
      std::cout
        << "\nVersion:\n"
        << "Log: " << (int) version.logVersion << "\n"
        << "Prg: " << (int) version.prgVerMaj << "." << (int) version.prgVerMin << "." << (int) version.prgVerRev << "\n" ;
      if (version.logVersion != LOG_DATA_VERSION)
        std::cerr << "log format version " << (int) version.logVersion << ", log-cvt was built for version " << LOG_DATA_VERSION << "\n" ;
    }
    break ;
  default:
    count.unknown += 1 ;
    std::cerr << "unknown type " << (int) header.pkt_type << " (" << header.body_size << " bytes) at offset " << start << ", skipped\n" ;
    break ;
  }

  return true ;
}

void usage()
{
  std::cout
    << "usage: log-cvt [-p|-b] [-u] <log-file> ...\n"
    << "options:\n"
    << "  -p    pcapng file\n"
    << "  -b    vector blf file\n"
    << "  -u    source is usb log with magic bytes (default sd log w/o magic bytes)\n"
    << "without -p or -b only the record counts are printed\n"
    << "exit status 1 if a file cannot be opened, read, converted or written\n"
    << "or contains no log records, else 0\n" ;
}

int main(int argc, char *argv[])
{
  int argi ;
  enum class Type { none, pcapng, blf } ;
  Type type{Type::none} ;
  int isUsbLog{0} ;
  int rc{0} ;

  for (argi = 1 ; argi < argc && argv[argi][0] == '-' ; ++argi)
  {
    std::string arg = argv[argi] ;
    if (arg == "-b")
      type = Type::blf ;
    else if (arg == "-p")
      type = Type::pcapng ;
    else if (arg == "-u")
      isUsbLog = 1 ;
    else
    {
      usage() ;
      return 1 ;
    }
  }

  if (argi == argc)
  {
    usage() ;
    return 1 ;
  }

  for ( ; argi < argc ; ++argi)
  {
    std::string logName = argv[argi] ;

    FILE *log = fopen(logName.c_str(), "rb") ;
    if (!log)
    {
      const char *err = strerror(errno) ;
      std::cerr << "open log " << logName << " failed: " << err << "\n" ;
      rc = 1 ;
      continue ;
    }

    std::unique_ptr<Writer> writer ;
    switch (type)
    {
    case Type::pcapng:
      writer = std::make_unique<PcapWriter>() ;
      break ;
    case Type::blf:
      writer = std::make_unique<BlfWriter>() ;
      break ;
    default:
      writer = std::make_unique<Writer>() ;
    }

    if (!writer->open(logName))
    {
      fclose(log) ;
      rc = 1 ;
      continue ;
    }

    std::cout
      << "\nFile: " << logName << "\n" ;

    ContentCount count ;
    while (!feof(log))
    {
      if ((isUsbLog && !skip_magic(log)) ||
          !log_cvt(log, isUsbLog, writer, count))
        break ;
    }

    if (ferror(log) || !feof(log))
    {
      std::cerr << "conversion of " << logName << " stopped at offset " << ftell(log) << "\n" ;
      rc = 1 ;
    }
    else if (count.time + count.its + count.gps + count.info + count.version == 0)
    {
      std::cerr << "no log records found in " << logName << " (wrong -u option?)\n" ;
      rc = 1 ;
    }

    std::cout
      << "\nCount:\n"
      // << "time: " << count.time << "\n"
      << "its: " << count.its << "\n"
      << "gps: " << count.gps << "\n"
      << "info: " << count.info << "\n"
      << "version: " << count.version << "\n" ;

    if (type == Type::none)
      std::cout << "\nno output file written (use -p or -b)\n" ;

    if (!writer->close())
      rc = 1 ;
    fclose(log) ;
  }
  return rc ;
}