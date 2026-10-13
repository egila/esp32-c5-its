#include "esp32-c5-its-otm.h"

template <class T>
bool read(QueueTty &queue, T &v)
{
  uint8_t *data = (uint8_t*)&v ;
  for (size_t i = 0 ; i < sizeof(v) ; ++i)
  {
    uint8_t b ;
    if (!queue.pop(b))
      return false ;
    data[i] = b ;
  }
  return true ;
}

bool read(QueueTty &queue, size_t size, std::vector<uint8_t> &v)
{
  v.resize(size) ;
  uint8_t *data = v.data() ;
  for (size_t i = 0 ; i < size ; ++i)
  {
    uint8_t b ;
    if (!queue.pop(b))
      return false ;
    data[i] = b ;
  }
  return true ;
}

bool skip_magic(QueueTty &queue, uint32_t &magic)
{
  uint8_t c ;

  if (magic == 0xaa5555aa)
    return true ;

  while (read(queue, c))
  {
    magic <<= 8 ;
    magic |= c ;
    if (magic == 0xaa5555aa)
      return true ;
  }
  return false ;
}

Parser::Parser(QueueTty &queueTty, QueueIts &queueIts) : _queueTty(queueTty), _queueIts(queueIts)
{
}

bool Parser::start()
{
  _thread = std::thread([this]()
  {
    Header header ;
    std::vector<uint8_t> body ;
    uint32_t shift{0} ;
    size_t drops{0} ;

    while (!shutdown)
    {
      // QueueTty drops bytes when it is full: note its drop count before a
      // search (the magic read after a record is covered by that record)
      if (shift != 0xaa5555aa)
        drops = _queueTty.drops() ;
      if (!skip_magic(_queueTty, shift))
        return ;
      shift = 0 ;

      if (!read(_queueTty, header))
        return ;

      if ((header.body_size > MAX_PKT_SIZE) || ((header.pkt_type == LOG_DATA_TYPE_ITS) && (header.body_size == 0)))
        continue ;

      if (!read(_queueTty, header.body_size, body))
        return ;
      // a record is complete only if the next record's magic follows it,
      // else drop it and resync, starting with the 4 bytes just read
      for (size_t i = 0 ; i < sizeof(shift) ; ++i)
      {
        uint8_t c ;
        if (!read(_queueTty, c))
          return ;
        shift <<= 8 ;
        shift |= c ;
      }
      if (shift != 0xaa5555aa)
        continue ;
      // bytes were dropped while the record was read, so it and the magic
      // after it may have gaps: drop the record and search for a new magic
      if (_queueTty.drops() != drops)
      {
        shift = 0 ;
        continue ;
      }

      std::cout << "\r\033[2K" << (uint32_t)header.pkt_type << " " << header.timestamp_us << " " << header.body_size ;
      std::cout.flush() ;

      switch (header.pkt_type)
      {
      case LOG_DATA_TYPE_TIME:
        break ;
      case LOG_DATA_TYPE_ITS:
        _queueIts.push(std::move(body)) ;
        break ;
      case LOG_DATA_TYPE_GPS:
        break ;
      case LOG_DATA_TYPE_INFO:
      std::cout
        << "\nInfo:\n"
        << std::string((char*)body.data(), body.size()) ;
        break ;
      case LOG_DATA_TYPE_VERSION:
        {
          if (body.size() < sizeof(Version))
            break ;
          const Version &version = *(Version*)body.data() ;
          std::cout
            << "\nVersion:\n"
            << "Log: " << (int) version.logVersion << "\n"
            << "Prg: " << (int) version.prgVerMaj << "." << (int) version.prgVerMin << "." << (int) version.prgVerRev << "\n" ;
        }
        break ;
      }
    }
   }) ;

  return true ;
}

void Parser::stop()
{
  if (_thread.joinable())
    _thread.join() ;
}
