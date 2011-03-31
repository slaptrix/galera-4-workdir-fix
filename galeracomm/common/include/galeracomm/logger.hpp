#ifndef _GCOMM_LOGGER_HPP_
#define _GCOMM_LOGGER_HPP_

#include <gu_logger.hpp>

#define LOG_TRACE(_a) 

#define LOG_DEBUG(_a) { log_debug << (_a); }
#define LOG_INFO(_a)  { log_info  << (_a); }
#define LOG_WARN(_a)  { log_warn  << (_a); }
#define LOG_ERROR(_a) { log_error << (_a); }
#define LOG_FATAL(_a) { log_fatal << (_a); }

#endif // _GCOMM_LOGGER_HPP_