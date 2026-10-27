#include "esp32-c5-its-otm.h"

Reader::Reader(QueueTty &queue) : _queue(queue), _port(-1)
{
}

bool Reader::initPort()
{
  _port = open(options._ttyName.c_str(), O_RDWR | O_NOCTTY);

  if (_port < 0) {
      std::cerr << "open failed " << options._ttyName << ": " << strerror(errno) << std::endl;
      return false;
  }

  // one bridge per receiver: do not reset the board under another instance
  if (flock(_port, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK) {
      std::cerr << "tty " << options._ttyName << " is in use by another process" << std::endl;
      close(_port);
      _port = -1;
      return false;
  }
  ioctl(_port, TIOCEXCL);

  struct termios tty;
  if (tcgetattr(_port, &tty) != 0) {
      std::cerr << "tcgetattr failed: " << strerror(errno) << std::endl;
      close(_port);
      _port = -1;
      return false;
  }

  cfmakeraw(&tty);
  tty.c_cflag |= HUPCL ;
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag |= CREAD | CLOCAL;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 10;

  cfsetispeed(&tty, B115200);
  cfsetospeed(&tty, B115200);

  if (tcsetattr(_port, TCSANOW, &tty) != 0)
  {
    std::cerr << "tcsetattr failed: " << strerror(errno) << std::endl;
    close(_port);
    _port = -1;
    return false;
  }

  int flags;
  if (ioctl(_port, TIOCMGET, &flags) >= 0)
  {
    flags &= ~TIOCM_DTR;
    flags |= TIOCM_RTS;
    ioctl(_port, TIOCMSET, &flags);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    flags |= TIOCM_DTR;
    flags |= TIOCM_RTS;
    ioctl(_port, TIOCMSET, &flags);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  else
  {
    std::cout << "ioctl failed: " << strerror(errno) << std::endl ;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  if (tcflush(_port, TCIFLUSH) != 0)
  {
    std::cerr << "tcflush failed: " << strerror(errno) << std::endl;
  }

  return true ;
}

void Reader::reopenPort()
{
  close(_port) ;
  _port = -1 ;
  _queue.clear() ;

  while (!shutdown)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1)) ;

    if (!shutdown && (access(options._ttyName.c_str(), F_OK) == 0) && initPort())
    {
      std::cerr << "tty " << options._ttyName << " reopened" << std::endl ;
      return ;
    }
  }
}

bool Reader::start()
{
  if (!initPort())
    return false ;

  _thread = std::thread([this]()
  {
    const std::chrono::seconds noDataMin(30), noDataMax(300) ;
    std::chrono::seconds noDataLimit = noDataMin ;
    auto lastData = std::chrono::steady_clock::now() ;

    auto lost = [this, &lastData](const char *reason, int err)
    {
      std::cerr << "\ntty " << options._ttyName << " lost: " << reason ;
      if (err)
        std::cerr << ": " << strerror(err) ;
      std::cerr << std::endl ;
      reopenPort() ;
      lastData = std::chrono::steady_clock::now() ;
    } ;

    uint8_t buff[256];
    while (!shutdown)
    {
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(_port, &fds);
      struct timeval timeout = { 1, 0 };

      int nSel = select(_port + 1, &fds, nullptr, nullptr, &timeout);

      if (nSel < 0) {
        if (errno != EINTR)
          lost("select failed", errno) ;
        continue;
      }

      if (nSel == 0) {
        if (std::chrono::steady_clock::now() - lastData > noDataLimit) {
          std::cerr << "\nno data from " << options._ttyName << " for " << noDataLimit.count() << " s, resetting the receiver" << std::endl ;
          reopenPort() ;
          lastData = std::chrono::steady_clock::now() ;
          noDataLimit *= 2 ;
          if (noDataLimit > noDataMax)
            noDataLimit = noDataMax ;
        }
        continue;
      }

      ssize_t nBuff = read(_port, &buff, sizeof(buff));

      if (nBuff == 0) {
        lost("hangup", 0) ;
        continue;
      }

      if (nBuff < 0) {
        if ((errno != EINTR) && (errno != EAGAIN))
          lost("read failed", errno) ;
        continue;
      }

      lastData = std::chrono::steady_clock::now() ;
      noDataLimit = noDataMin ;

      for (ssize_t iBuff = 0; iBuff < nBuff; ++iBuff) {
        _queue.push(buff[iBuff]) ;
      }
    }
  }) ;

  return true ;
}

void Reader::stop()
{
  if (_thread.joinable())
    _thread.join() ;

  if (_port >= 0)
    close(_port);
}