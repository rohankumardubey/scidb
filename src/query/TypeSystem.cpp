/*
**
* BEGIN_COPYRIGHT
*
* This file is part of SciDB.
* Copyright (C) 2008-2014 SciDB, Inc.
*
* SciDB is free software: you can redistribute it and/or modify
* it under the terms of the AFFERO GNU General Public License as published by
* the Free Software Foundation.
*
* SciDB is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
* INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,
* NON-INFRINGEMENT, OR FITNESS FOR A PARTICULAR PURPOSE. See
* the AFFERO GNU General Public License for the complete license terms.
*
* You should have received a copy of the AFFERO GNU General Public License
* along with SciDB.  If not, see <http://www.gnu.org/licenses/agpl-3.0.html>
*
* END_COPYRIGHT
*/

/*
 * @file TypeSystem.cpp
 *
 * @author roman.simakov@gmail.com
 */

#include <stdio.h>
#include <inttypes.h>
#include <iomanip>
#include <stdarg.h>
#include <float.h>
#include <vector>
#include <boost/algorithm/string.hpp>

#include "log4cxx/logger.h"
#include "log4cxx/basicconfigurator.h"
#include "log4cxx/helpers/exception.h"

#include "query/TypeSystem.h"
#include "util/PluginManager.h"
#include "query/FunctionLibrary.h"
#include "query/LogicalExpression.h"

using namespace std;
using namespace boost;

namespace scidb
{

// to prevent visibility of variable outside of file
static log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("scidb.typesystem"));

std::ostream& operator<<(std::ostream& stream, const Type& ob )
{
    stream << ob.typeId();
    return stream;
}

//
// PGB: Note that this will only generate the subset of the input list
//      of types that are actually in the TypeLibrary.
std::ostream& operator<<(std::ostream& stream,
                         const std::vector< TypeId>& obs )
{
    for (size_t i = 0, l = obs.size(); i < l; i++) {
        if (i) {
            stream << ", " << TypeLibrary::getType(obs[i]);
        } else {
            stream << " " << TypeLibrary::getType(obs[i]);
        }
    }
    return stream;
}

std::ostream& operator<<(std::ostream& stream,
                         const std::vector< Type>& obs )
{
    for (size_t i = 0, l = obs.size(); i < l; i++) {
        if (i) {
            stream << ", " << obs[i];
        } else {
            stream << " " << obs[i];
        }
    }
    return stream;
}

bool Type::isSubtype(TypeId const& subtype, TypeId const& supertype)
{
    return TypeLibrary::getType(subtype).isSubtypeOf(supertype);
}

std::ostream& operator<<(std::ostream& os, const Value& ob )
{
    // We could do a whole lot better here, but at the very least, for
    // data of manageable sizes let's print out the bit pattern.

    os << "scidb::Value(";
    switch (ob.size()) {
    case 1:
        os << "0x" << hex << setfill('0') 
           << *static_cast<uint8_t*>(ob.data()) << dec;
        if (ob.getMissingReason() != -1)
            os << ", missingReason=" << ob.getMissingReason();
        break;
    case 2:
        os << "0x" << hex << setfill('0') 
           << *static_cast<uint16_t*>(ob.data()) << dec;
        if (ob.getMissingReason() != -1)
            os << ", missingReason=" << ob.getMissingReason();
        break;
    case 4:
        os << "0x" << hex << setfill('0') 
           << *static_cast<uint32_t*>(ob.data()) << dec;
        if (ob.getMissingReason() != -1)
            os << ", missingReason=" << ob.getMissingReason();
        break;
    case 8:
        os << "0x" << hex << setfill('0')
           << *static_cast<uint64_t*>(ob.data()) << dec;
        if (ob.getMissingReason() != -1)
            os << ", missingReason=" << ob.getMissingReason();
        break;
    default:
        os << "size=" << ob.size()
           << ", data=" << ob.data()
           << ", missingReason=" << ob.getMissingReason();
        break;
    }
    os << ')';
    return os;
}

/**
 * TypeLibrary implementation
 */

/*
** This is the list of type names and the bit-sizes for the built-in type
** list.
*/
static struct BuiltinInfo {
    const char* name;
    size_t      bits;
} const builtinTypeInfo[] = {
    { TID_INDICATOR,    1 },
    { TID_CHAR,         8 },
    { TID_INT8,         8 },
    { TID_INT16,        16 },
    { TID_INT32,        32 },
    { TID_INT64,        64 },
    { TID_UINT8,        8 },
    { TID_UINT16,       16 },
    { TID_UINT32,       32 },
    { TID_UINT64,       64 },
    { TID_FLOAT,        32 },
    { TID_DOUBLE,       64 },
    { TID_BOOL,         1 },
    { TID_STRING,       0 },
    { TID_DATETIME,     sizeof(time_t) * 8 },
    { TID_VOID,         0 },
    { TID_BINARY,       0 },
    { TID_DATETIMETZ,   2 * sizeof(time_t) * 8 }
    // TID_FIXED_STRING intentionally left out, see below.
};

#define BUILTIN_INFO_COUNT      SCIDB_SIZE(builtinTypeInfo)


TypeLibrary TypeLibrary::_instance;

TypeLibrary::TypeLibrary()
{
#ifdef SCIDB_CLIENT
    registerBuiltInTypes();
#endif
}

void TypeLibrary::registerBuiltInTypes()
{
    for (unsigned i = 0; i < BUILTIN_INFO_COUNT; i++) {
        const BuiltinInfo& bti = builtinTypeInfo[i];
        Type type(bti.name, bti.bits);
        Value _defaultValue(type);
        _instance._registerType(type);
        _instance._builtinTypesById[bti.name] = type;
        _instance._defaultValuesById[bti.name] = _defaultValue;
    }
}

bool TypeLibrary::_hasType(TypeId typeId)
{
    if (_builtinTypesById.find(typeId) != _builtinTypesById.end()) {
        return true;
    } else {
        ScopedMutexLock cs(mutex);
        return _typesById.find(typeId) != _typesById.end();
    }
}

const Type& TypeLibrary::_getType(TypeId typeId)
{
    map<TypeId, Type, __lesscasecmp>::const_iterator i = _builtinTypesById.find(typeId);
    if (i != _builtinTypesById.end()) {
        return i->second;
    } else {
        ScopedMutexLock cs(mutex);
        i = _typesById.find(typeId);
        if (i == _typesById.end()) {
            size_t pos = typeId.find_first_of('_');
            if (pos != string::npos) {
                string genericTypeId = typeId.substr(0, pos+1) + '*';
                i = _typesById.find(genericTypeId);
                if (i != _typesById.end()) {
                    Type limitedType(typeId, atoi(typeId.substr(pos+1).c_str())*8, i->second.baseType());
                    _typeLibraries.addObject(typeId);
                    return _typesById[typeId] = limitedType;
                }
            }
            LOG4CXX_DEBUG(logger, "_getType('" << typeId << "') not found");
            throw SYSTEM_EXCEPTION(SCIDB_SE_TYPESYSTEM, SCIDB_LE_TYPE_NOT_REGISTERED) << typeId;
        }
        return i->second;
    }
}

void TypeLibrary::_registerType(const Type& type)
{
    ScopedMutexLock cs(mutex);
    map<string, Type, __lesscasecmp>::const_iterator i = _typesById.find(type.typeId());
    if (i == _typesById.end()) {
        _typesById[type.typeId()] = type;
        _typeLibraries.addObject(type.typeId());
    } else {
        if (i->second.bitSize() != type.bitSize() || i->second.baseType() != type.baseType())  {
            throw SYSTEM_EXCEPTION(SCIDB_SE_TYPESYSTEM, SCIDB_LE_TYPE_ALREADY_REGISTERED) << type.typeId();
        }
    }
}

size_t TypeLibrary::_typesCount()
{
    ScopedMutexLock cs(mutex);
    size_t count = 0;
    for (map<string, Type, __lesscasecmp>::const_iterator i = _typesById.begin();
         i != _typesById.end();
         ++i)
    {
        if (i->first[0] != '$')
            ++count;
    }
    return count;
}

std::vector<TypeId> TypeLibrary::_typeIds()
{
    ScopedMutexLock cs(mutex);
    std::vector<std::string> list;
    for (map<string, Type, __lesscasecmp>::const_iterator i = _typesById.begin(); i != _typesById.end();
         ++i)
    {
        if (i->first[0] != '$')
            list.push_back(i->first);
    }
    return list;
}

const Value& TypeLibrary::_getDefaultValue(TypeId typeId)
{
    std::map<TypeId, Value, __lesscasecmp>::iterator iter = _defaultValuesById.find(typeId);
    if (iter != _defaultValuesById.end())
    {
        return iter->second;
    }

    Value _defaultValue(_getType(typeId));

    FunctionDescription functionDesc;
    vector<FunctionPointer> converters;
    if (FunctionLibrary::getInstance()->findFunction(typeId, vector<TypeId>(), functionDesc, converters, false))
    {
        functionDesc.getFuncPtr()(0, &_defaultValue, 0);
    }
    else
    {
        stringstream ss;
        ss << typeId << "()";
        throw USER_EXCEPTION(SCIDB_SE_QPROC, SCIDB_LE_FUNCTION_NOT_FOUND) << ss.str();
    }

    return _defaultValuesById[typeId] = _defaultValue;
}

/**
 * Quote a string as a TID_STRING ought to be quoted.
 *
 * @description
 * Copy a string to an output stream, inserting backslashes before
 * characters in the quoteThese string.
 *
 * @param os the output stream
 * @param s the string to copy out
 * @param quoteThese string of characters requiring backslashes
 */
static void
tidStringQuote(std::ostream& os, const char *s, const char *quoteThese)
{
    char ch;
    const char *cp = s;
    while ((ch = *cp++)) {
        if (::index(quoteThese, ch))
            os << '\\';
        os << ch;
    }
}

/**
 * Helper Value functions implementation
 *
 * NOTE: This will only work efficiently for the built in types. If you try
 *       use this for a UDT it needs to do a lookup to try and find a UDF.
 */
string ValueToString(const TypeId type, const Value& value, int precision)
{
    std::stringstream ss;

    /*
    ** Start with the most common ones, and do the least common ones
    ** last.
    */
    if ( value.isNull() ) {
        if (value.getMissingReason() == 0) {
            ss << "null";
        } else {
            ss << '?' << value.getMissingReason();
        }
    } else if ( TID_DOUBLE == type ) {
        double val = value.getDouble();
        if (isnan(val) || val==0)
            val = abs(val);
        ss.precision(precision);
        ss << val;
    } else if ( TID_INT64 == type ) {
        ss << value.getInt64();
    } else if ( TID_INT32 == type ) {
        ss << value.getInt32();
    } else if ( TID_STRING == type ) {
        char const* str = value.getString();
        if (str == NULL) {
            ss << "null";
        } else {
            ss << '\'';
            tidStringQuote(ss, str, "\\'");
            ss << '\'';
        }
    } else if ( TID_CHAR == type ) {

        ss << '\'';
        const char ch = value.getChar();
        if (ch == '\0') {
                ss << "\\0";
        } else if (ch == '\n') {
                ss << "\\n";
        } else if (ch == '\r') {
                ss << "\\r";
        } else if (ch == '\t') {
                ss << "\\t";
        } else if (ch == '\f') {
                ss << "\\f";
        } else {
                if (ch == '\'' || ch == '\\') {
                ss << '\\';
                }
                ss << ch;
        }
        ss << '\'';

    } else if ( TID_FLOAT == type ) {
        ss << value.getFloat();
    } else if (( TID_BOOL == type ) || ( TID_INDICATOR == type )) {
        ss << (value.getBool() ? "true" : "false");
    } else if ( TID_DATETIME == type ) {

        char buf[STRFTIME_BUF_LEN];
        struct tm tm;
        time_t dt = (time_t)value.getDateTime();

        gmtime_r(&dt, &tm);
        strftime(buf, sizeof(buf), DEFAULT_STRFTIME_FORMAT, &tm);
        ss << '\'' << buf << '\'';

    } else if ( TID_DATETIMETZ == type) {

            char buf[STRFTIME_BUF_LEN + 8];
            time_t *seconds = (time_t*) value.data();
            time_t *offset = seconds+1;

            struct tm tm;
            gmtime_r(seconds,&tm);
            size_t offs = strftime(buf, sizeof(buf), DEFAULT_STRFTIME_FORMAT, &tm);

            char sign = *offset > 0 ? '+' : '-';

            time_t aoffset = *offset > 0 ? *offset : (*offset) * -1;

            sprintf(buf+offs, " %c%02d:%02d",
                    sign,
                    (int32_t) aoffset/3600,
                    (int32_t) (aoffset%3600)/60);


            ss << '\'' << buf << '\'';
    } else if ( TID_INT8 == type ) {
        ss << (int)value.getInt8();
    } else if ( TID_INT16 == type ) {
        ss << value.getInt16();
    } else if ( TID_UINT8 == type ) {
        ss << (int)value.getUint8();
    } else if ( TID_UINT16 == type ) {
        ss << value.getUint16();
    } else if ( TID_UINT32 == type ) {
        ss << value.getUint32();
    } else if ( TID_UINT64 == type ) {
        ss << value.getUint64();
    } else if ( TID_VOID == type ) {
        ss << "<void>";
    } else  {
        ss << "<" << type << ">";
    }

    return ss.str();
}

inline void mStringToMonth(char* mString, int& month)
{
    if (boost::iequals(mString, "jan"))
    {
        month = 1;
    }
    else if (boost::iequals(mString, "feb"))
    {
        month = 2;
    }
    else if (boost::iequals(mString, "mar"))
    {
        month = 3;
    }
    else if (boost::iequals(mString, "apr"))
    {
        month = 4;
    }
    else if (boost::iequals(mString, "may"))
    {
        month = 5;
    }
    else if (boost::iequals(mString, "jun"))
    {
        month = 6;
    }
    else if (boost::iequals(mString, "jul"))
    {
        month = 7;
    }
    else if (boost::iequals(mString, "aug"))
    {
        month = 8;
    }
    else if (boost::iequals(mString, "sep"))
    {
        month = 9;
    }
    else if (boost::iequals(mString, "oct"))
    {
        month = 10;
    }
    else if (boost::iequals(mString, "nov"))
    {
        month = 11;
    }
    else if (boost::iequals(mString, "dec"))
    {
        month = 12;
    }
    else
    {
        throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_INVALID_MONTH_REPRESENTATION) << string(mString);
    }
}

/**
 * Parse a string that contains (hopefully) a DateTime constant into
 * the internal representation.
 * @param string containing DateTime value
 * @return standard time_t.
 */
time_t parseDateTime(std::string const& str)
{
    struct tm t;
    time_t now = time(NULL);
    if (str == "now") {
        return now;
    }
    gmtime_r(&now, &t);
    int n;
    int sec_frac;
    char const* s = str.c_str();
    t.tm_mon += 1;
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    char mString[4]="";
    char amPmString[3]="";

    if (( sscanf(s, "%d-%3s-%d %d.%d.%d %2s%n", &t.tm_mday, &mString[0], &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &amPmString[0], &n) == 7 ||
          sscanf(s, "%d-%3s-%d %d.%d.%d%n", &t.tm_mday, &mString[0], &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &n) == 6 ||
          sscanf(s, "%d-%3s-%d%n", &t.tm_mday, &mString[0], &t.tm_year, &n) == 3 ||
          sscanf(s, "%d%3s%d:%d:%d:%d%n", &t.tm_mday, &mString[0], &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &n) == 6 ) && n == (int) str.size())
    {
        mStringToMonth(&mString[0], t.tm_mon);
        if(amPmString[0]=='P')
        {
            t.tm_hour += 12;
        }
    }
    else
    {
        if((sscanf(s, "%d/%d/%d %d:%d:%d%n", &t.tm_mon, &t.tm_mday, &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &n) != 6 &&
                    sscanf(s, "%d.%d.%d %d:%d:%d%n", &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &n) != 6 &&
                    sscanf(s, "%d-%d-%d %d:%d:%d.%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec, &sec_frac, &n) != 7 &&
                    sscanf(s, "%d-%d-%d %d.%d.%d.%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec, &sec_frac, &n) != 7 &&
                    sscanf(s, "%d-%d-%d %d.%d.%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec, &n) != 6 &&
                    sscanf(s, "%d-%d-%d %d:%d:%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec, &n) != 6 &&
                    sscanf(s, "%d/%d/%d %d:%d%n", &t.tm_mon, &t.tm_mday, &t.tm_year, &t.tm_hour, &t.tm_min, &n) != 5 &&
                    sscanf(s, "%d.%d.%d %d:%d%n", &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_hour, &t.tm_min, &n) != 5 &&
                    sscanf(s, "%d-%d-%d %d:%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &n) != 5 &&
                    sscanf(s, "%d-%d-%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &n) != 3 &&
                    sscanf(s, "%d/%d/%d%n", &t.tm_mon, &t.tm_mday, &t.tm_year, &n) != 3 &&
                    sscanf(s, "%d.%d.%d%n", &t.tm_mday, &t.tm_mon, &t.tm_year, &n) != 3 &&
                    sscanf(s, "%d:%d:%d%n", &t.tm_hour, &t.tm_min, &t.tm_sec, &n) != 3 &&
                    sscanf(s, "%d:%d%n", &t.tm_hour, &t.tm_min, &n) != 2)
                    || n != (int)str.size())
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << TID_DATETIME;
    }

    if (!(t.tm_mon >= 1 && t.tm_mon <= 12  && t.tm_mday >= 1 && t.tm_mday <= 31 && t.tm_hour >= 0
          && t.tm_hour <= 23 && t.tm_min >= 0 && t.tm_min <= 59 && t.tm_sec >= 0 && t.tm_sec <= 60))
        throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_INVALID_SPECIFIED_DATE);

    t.tm_mon -= 1;
    if (t.tm_year >= 1900) {
        t.tm_year -= 1900;
    } else if (t.tm_year < 100) {
        t.tm_year += 100;
    }
    return timegm(&t);
}

void parseDateTimeTz(std::string const& str, Value& result)
{
    if (str == "now")
    {
        pair<time_t,time_t> r;
        time_t now = time(NULL);
        struct tm localTm;
        localtime_r(&now, &localTm);
        r.second = timegm(&localTm) - now;
        r.first = now + r.second;
        result.setData(&r, 2*sizeof(time_t));
    }

    struct tm t;
    int offsetHours, offsetMinutes, secFrac, n;
    char mString[4]="";
    char amPmString[3]="";

    char const* s = str.c_str();
    t.tm_mon += 1;
    t.tm_hour = t.tm_min = t.tm_sec = 0;

    if ((sscanf(s, "%d-%3s-%d %d.%d.%d %2s %d:%d%n", &t.tm_mday, &mString[0], &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &amPmString[0], &offsetHours, &offsetMinutes, &n) == 9)
        && n == (int)str.size())
    {
        mStringToMonth(&mString[0], t.tm_mon);
        if(amPmString[0]=='P')
        {
            t.tm_hour += 12;
        }
    }
    else
    {
        if((sscanf(s, "%d/%d/%d %d:%d:%d %d:%d%n", &t.tm_mon, &t.tm_mday, &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &offsetHours, &offsetMinutes, &n) != 8 &&
                sscanf(s, "%d.%d.%d %d:%d:%d %d:%d%n", &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &offsetHours, &offsetMinutes, &n) != 8 &&
                sscanf(s, "%d-%d-%d %d:%d:%d.%d %d:%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec, &secFrac, &offsetHours, &offsetMinutes, &n) != 9 &&
                sscanf(s, "%d-%d-%d %d:%d:%d %d:%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec, &offsetHours, &offsetMinutes, &n) != 8 &&
                sscanf(s, "%d-%d-%d %d.%d.%d.%d %d:%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec, &secFrac, &offsetHours, &offsetMinutes, &n) != 9 &&
                sscanf(s, "%d-%d-%d %d.%d.%d %d:%d%n", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec, &offsetHours, &offsetMinutes, &n) != 8 &&
                sscanf(s, "%d-%3s-%d %d.%d.%d %2s %d:%d%n", &t.tm_mday, &mString[0], &t.tm_year, &t.tm_hour, &t.tm_min, &t.tm_sec, &amPmString[0], &offsetHours, &offsetMinutes, &n) != 9)
              || n != (int)str.size())
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << TID_DATETIMETZ;
    }

    if (offsetHours < 0 && offsetMinutes > 0)
    {
        offsetMinutes *= -1;
    }

    if (!(t.tm_mon >= 1 && t.tm_mon <= 12  &&
          t.tm_mday >= 1 && t.tm_mday <= 31 &&
          t.tm_hour >= 0 && t.tm_hour <= 23 &&
          t.tm_min >= 0 && t.tm_min <= 59 &&
          t.tm_sec >= 0 && t.tm_sec <= 60 &&
          offsetHours>=-13 && offsetHours<=13 &&
          offsetMinutes>=-59 && offsetMinutes<=59))
        throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_INVALID_SPECIFIED_DATE);

    t.tm_mon -= 1;
    if (t.tm_year >= 1900) {
        t.tm_year -= 1900;
    } else if (t.tm_year < 100) {
        t.tm_year += 100;
    }

    pair<time_t,time_t> r;
    r.first = timegm(&t);
    r.second = (offsetHours * 3600 + offsetMinutes * 60);
    result.setData(&r, 2*sizeof(time_t));
}

bool isBuiltinType(const TypeId type)
{
        return TID_DOUBLE == type
        || TID_INT64 == type
        || TID_INT32 == type
        || TID_CHAR == type
        || TID_STRING == type
        || TID_FLOAT == type
        || TID_INT8 == type
        || TID_INT16 == type
        || TID_UINT8 == type
        || TID_UINT16 == type
        || TID_UINT32 == type
        || TID_UINT64 == type
        || TID_INDICATOR == type
        || TID_BOOL == type
        || TID_DATETIME == type
        || TID_VOID == type
        || TID_DATETIMETZ == type
        || TID_BINARY == type;
}

TypeId propagateType(const TypeId type)
{
        return TID_INT8 == type || TID_INT16 == type || TID_INT32 == type
        ? TID_INT64
        : TID_UINT8 == type || TID_UINT16 == type || TID_UINT32 == type
        ? TID_UINT64
        : TID_FLOAT == type ? TID_DOUBLE : type;
}

TypeId propagateTypeToReal(const TypeId type)
{
        return TID_INT8 == type || TID_INT16 == type || TID_INT32 == type || TID_INT64 == type
        || TID_UINT8 == type || TID_UINT16 == type || TID_UINT32 == type || TID_UINT64 == type
        || TID_FLOAT == type ? TID_DOUBLE : type;
}

// Use of "%"PRIi64"%n" as a sscanf format string for parsing int64
// values is problematic because it treats numbers with leading zeroes
// as octal, so that for example the ZIP code 02139 is not parseable
// since 9 is not an octal digit.  This routine gets rid of leading
// zeroes to prevent this non-intuitive behavior.  See ticket #4273.
//
#ifdef FIX_TICKET_4273
static string stripLeadingZeroes(const string& str)
{
    const char* cp = str.c_str();
    if (*cp == '0' && ::tolower(*(cp + 1)) != 'x') {
        while (*cp == '0')
            ++cp;
        if (!::isdigit(*cp))
            --cp;
        return string(cp);
    }
    return str;
}
#endif

void StringToValue(const TypeId type, const string& str, Value& value)
{
    int n;
    if ( TID_DOUBLE == type ) {
        if (str == "NA") {
            // backward compatibility
            value.setDouble(NAN);
        } else {
            value.setDouble(atof(str.c_str()));
        }
    } else if ( TID_INT64 == type ) {
        int64_t val;
#ifdef FIX_TICKET_4273
        string str1 = stripLeadingZeroes(str);
#else
        const string& str1 = str;
#endif
        if (sscanf(str1.c_str(), "%"PRIi64"%n", &val, &n) != 1 || n != (int)str1.size())
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << type;
        value.setInt64(val);
    } else if ( TID_INT32 == type ) {
        int val;
        if (sscanf(str.c_str(), "%d%n", &val, &n) != 1 || n != (int)str.size())
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << type;
        value.setInt32(val);
    } else if (  TID_CHAR == type )  {
        value.setChar(str[0]);
    } else if ( TID_STRING == type ) {
        value.setString(str.c_str());
    } else if ( TID_FLOAT == type ) {
        if (str == "NA") {
            // backward compatibility
            value.setFloat(NAN);
        } else {
            value.setFloat(atof(str.c_str()));
        }
    } else if ( TID_INT8 == type ) {
        int16_t val;
        if (sscanf(str.c_str(), "%hd%n", &val, &n) != 1 || n != (int)str.size() || val>127 || val<-127)
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << type;
        value.setInt8(static_cast<int8_t>(val));
    } else if (TID_INT16 == type) {
        int16_t val;
        if (sscanf(str.c_str(), "%hd%n", &val, &n) != 1 || n != (int)str.size())
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << type;
        value.setInt16(val);
    } else if ( TID_UINT8 == type ) {
        uint16_t val;
        if (sscanf(str.c_str(), "%hu%n", &val, &n) != 1 || n != (int)str.size() || val>255)
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << type;
        value.setUint8(static_cast<uint8_t>(val));
    } else if ( TID_UINT16 == type ) {
        uint16_t val;
        if (sscanf(str.c_str(), "%hu%n", &val, &n) != 1 || n != (int)str.size())
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << type;
        value.setUint16(val);
    } else if ( TID_UINT32 == type ) {
        unsigned val;
        if (sscanf(str.c_str(), "%u%n", &val, &n) != 1 || n != (int)str.size())
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << type;
        value.setUint32(val);
    } else if ( TID_UINT64 == type ) {
        uint64_t val;
        if (sscanf(str.c_str(), "%"PRIu64"%n", &val, &n) != 1 || n != (int)str.size())
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << type;
        value.setUint64(val);
    } else if (( TID_INDICATOR == type ) || ( TID_BOOL == type )) {
        if (str == "true") {
            value.setBool(true);
        } else if (str == "false") {
            value.setBool(false);
        } else {
            throw SYSTEM_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_TYPE_CONVERSION_ERROR2)
                << str << "string" << "bool";
        }
    } else if ( TID_DATETIME == type ) {
        value.setDateTime(parseDateTime(str));
    } else if ( TID_DATETIMETZ == type) {
        parseDateTimeTz(str, value);
    } else if ( TID_VOID == type ) {
        throw SYSTEM_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_TYPE_CONVERSION_ERROR2)
            << str << "string" << type;
    } else {
        std::stringstream ss;
        ss << type;
        throw SYSTEM_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_TYPE_CONVERSION_ERROR2)
            << str << "string" << type;
    }
}

double ValueToDouble(const TypeId type, const Value& value)
{
    std::stringstream ss;
    if ( TID_DOUBLE == type ) {
        return value.getDouble();
    } else if ( TID_INT64 == type ) {
        return value.getInt64();
    } else if ( TID_INT32 == type ) {
        return value.getInt32();
    } else if ( TID_CHAR == type ) {
        return value.getChar();
    } else if ( TID_STRING == type ) {
        double d;
        int n;
        char const* str = value.getString();
        if (sscanf(str, "%lf%n", &d, &n) != 1 || n != (int)strlen(str))
            throw USER_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_FAILED_PARSE_STRING) << str << "double";
        return d;
    } else if ( TID_FLOAT == type ) {
        return value.getFloat();
    } else if ( TID_INT8 == type ) {
        return value.getInt8();
    } else if ( TID_INT16 == type ) {
        return value.getInt16();
    } else if ( TID_UINT8 == type ) {
        return value.getUint8();
    } else if ( TID_UINT16 == type ) {
        return value.getUint16();
    } else if ( TID_UINT32 == type ) {
        return value.getUint32();
    } else if ( TID_UINT64 == type ) {
        return value.getUint64();
    } else if (( TID_INDICATOR == type ) || ( TID_BOOL == type )) {
        return value.getBool();
    } else if ( TID_DATETIME == type ) {
        return value.getDateTime();
    } else {
        throw SYSTEM_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_TYPE_CONVERSION_ERROR)
            << type << "double";
    }
}

void DoubleToValue(const TypeId type, double d, Value& value)
{
      if (  TID_DOUBLE == type ) {
        value.setDouble(d);
      } else if ( TID_INT64 == type ) {
        value.setInt64((int64_t)d);
      } else if ( TID_UINT32 == type ) {
        value.setUint32((uint32_t)d);
      } else if ( TID_CHAR == type ) {
        value.setChar((char)d);
      } else if ( TID_FLOAT == type ) {
        value.setFloat((float)d);
      } else if ( TID_INT8 == type ) {
        value.setInt8((int8_t)d);
      } else if ( TID_INT16 == type ) {
        value.setInt32((int32_t)d);
      } else if ( TID_UINT8 == type ) {
        value.setUint8((uint8_t)d);
      } else if ( TID_UINT16 == type ) {
        value.setUint16((uint16_t)d);
      } else if ( TID_UINT64 == type ) {
        value.setUint64((uint64_t)d);
      } else if (( TID_INDICATOR == type ) || ( TID_BOOL == type )) {
        value.setBool(d != 0.0);
      } else if ( TID_STRING == type ) {
          std::stringstream ss;
          ss << d;
          value.setString(ss.str().c_str());
      } else if (  TID_DATETIME == type ) {
        return value.setDateTime((time_t)d);
    } else {
        throw SYSTEM_EXCEPTION(SCIDB_SE_TYPE_CONVERSION, SCIDB_LE_TYPE_CONVERSION_ERROR)
            << "double" << type;
    }
}

void Value::makeTileConstant(const TypeId& typeId)
{
    assert(_tile == NULL);

    RLEPayload& p = *getTile(typeId);
    RLEPayload::Segment s;
    s._same = true;
    s._null = isNull();
    s._pPosition = 0;
    s._valueIndex = 0;
    if (!s._null) {
        std::vector<char> varPart;
        p.appendValue(varPart, *this, 0);
        p.setVarPart(varPart);
    }
    p.addSegment(s);
    p.flush(INFINITE_LENGTH);
}

template<>  TypeId type2TypeId<char>() { return TID_CHAR; }
template<>  TypeId type2TypeId<int8_t>() { return TID_INT8; }
template<>  TypeId type2TypeId<int16_t>() { return TID_INT16; }
template<>  TypeId type2TypeId<int32_t>() { return TID_INT32; }
template<>  TypeId type2TypeId<int64_t>() { return TID_INT64; }
template<>  TypeId type2TypeId<uint8_t>() { return TID_UINT8; }
template<>  TypeId type2TypeId<uint16_t>() { return TID_UINT16; }
template<>  TypeId type2TypeId<uint32_t>() { return TID_UINT32; }
template<>  TypeId type2TypeId<uint64_t>() { return TID_UINT64; }
template<>  TypeId type2TypeId<float>() { return TID_FLOAT; }
template<>  TypeId type2TypeId<double>() { return TID_DOUBLE; }




} // namespace

