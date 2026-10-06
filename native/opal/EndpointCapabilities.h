#pragma once
#include <h323/h323ep.h>
namespace studio::opal {
inline void ConfigureH323Content(H323EndPoint&endpoint){endpoint.SetDefaultH239Control(true);}
}
