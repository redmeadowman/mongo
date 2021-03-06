/** @file jsobj.cpp - BSON implementation
    http://www.mongodb.org/display/DOCS/BSON
*/

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "../bson/oid.h"
#include "jsobj.h"
#include "nonce.h"
#include "../bson/util/atomic_int.h"
#include "../util/base64.h"
#include "../util/md5.hpp"
#include <limits>
#include "../util/unittest.h"
#include "../util/embedded_builder.h"
#include "../util/stringutils.h"
#include "json.h"
#include "jsobjmanipulator.h"
#include "../util/optime.h"
#include "../util/ip_addr.h"
#include "../util/mac_addr.h"
#include <boost/static_assert.hpp>
#undef assert
#define assert MONGO_assert

// make sure our assumptions are valid
BOOST_STATIC_ASSERT( sizeof(short) == 2 );
BOOST_STATIC_ASSERT( sizeof(int) == 4 );
BOOST_STATIC_ASSERT( sizeof(long long) == 8 );
BOOST_STATIC_ASSERT( sizeof(double) == 8 );
BOOST_STATIC_ASSERT( sizeof(mongo::Date_t) == 8 );
BOOST_STATIC_ASSERT( sizeof(mongo::OID) == 12 );

namespace mongo {

    BSONElement nullElement;

    GENOIDLabeler GENOID;

    DateNowLabeler DATENOW;

    MinKeyLabeler MINKEY;
    MaxKeyLabeler MAXKEY;

    // need to move to bson/, but has dependency on base64 so move that to bson/util/ first.
    inline string BSONElement::jsonString( JsonStringFormat format, bool includeFieldNames, int pretty ) const {
        BSONType t = type();
        if ( t == Undefined )
            return "undefined";

        stringstream s;
        if ( includeFieldNames )
            s << '"' << escape( fieldName() ) << "\" : ";
        switch ( type() ) {
        case mongo::String:
        case Symbol:
            s << '"' << escape( string(valuestr(), valuestrsize()-1) ) << '"';
            break;
        case NumberLong:
            s << _numberLong();
            break;
        case NumberInt:
        case NumberDouble:
            if ( number() >= -numeric_limits< double >::max() &&
                    number() <= numeric_limits< double >::max() ) {
                s.precision( 16 );
                s << number();
            }
            else {
                StringBuilder ss;
                ss << "Number " << number() << " cannot be represented in JSON";
                string message = ss.str();
                massert( 10311 ,  message.c_str(), false );
            }
            break;
        case mongo::Bool:
            s << ( boolean() ? "true" : "false" );
            break;
        case jstNULL:
            s << "null";
            break;
        case Object:
            s << embeddedObject().jsonString( format, pretty );
            break;
        case mongo::Array: {
            if ( embeddedObject().isEmpty() ) {
                s << "[]";
                break;
            }
            s << "[ ";
            BSONObjIterator i( embeddedObject() );
            BSONElement e = i.next();
            if ( !e.eoo() ) {
                int count = 0;
                while ( 1 ) {
                    if( pretty ) {
                        s << '\n';
                        for( int x = 0; x < pretty; x++ )
                            s << "  ";
                    }

                    if (strtol(e.fieldName(), 0, 10) > count) {
                        s << "undefined";
                    }
                    else {
                        s << e.jsonString( format, false, pretty?pretty+1:0 );
                        e = i.next();
                    }
                    count++;
                    if ( e.eoo() )
                        break;
                    s << ", ";
                }
            }
            s << " ]";
            break;
        }
        case DBRef: {
            mongo::OID *x = (mongo::OID *) (valuestr() + valuestrsize());
            if ( format == TenGen )
                s << "Dbref( ";
            else
                s << "{ \"$ref\" : ";
            s << '"' << valuestr() << "\", ";
            if ( format != TenGen )
                s << "\"$id\" : ";
            s << '"' << *x << "\" ";
            if ( format == TenGen )
                s << ')';
            else
                s << '}';
            break;
        }
        case jstOID:
            if ( format == TenGen ) {
                s << "ObjectId( ";
            }
            else {
                s << "{ \"$oid\" : ";
            }
            s << '"' << __oid() << '"';
            if ( format == TenGen ) {
                s << " )";
            }
            else {
                s << " }";
            }
            break;
        case BinData: {
            int len = *(int *)( value() );
            BinDataType type = BinDataType( *(char *)( (int *)( value() ) + 1 ) );
            char *start = ( char * )( value() ) + sizeof( int ) + 1;
            switch (type) {
            case bdtUUID:
                s << "{ \"$uuid\" : \"";
                s << toHex(start, 16);
                s << "\" }";
                break;
            case bdtIpAddr: {
                const IP_Addr ip(start, len);
                s << "{ \"$ipaddr\" : \"";
                s << ip.print();
                s << "\" }";
                break;
                }
            case bdtMacAddr: {
                const MAC_Addr* mac = new ((void*)start) MAC_Addr;
                s << "{ \"$macaddr\" : \"";
                s << mac->print();
                s << "\" }";
                break;
                }
            default: {
                s << "{ \"$binary\" : \"";
                base64::encode( s , start , len );
                s << "\", \"$type\" : \"" << hex;
                s.width( 2 );
                s.fill( '0' );
                s << type << dec;
                s << "\" }";
                break;
                }
            } // end switch
            break;
        }
        case mongo::Date:
            if ( format == Strict )
                s << "{ \"$date\" : ";
            else
                s << "Date( ";
            if( pretty ) {
                Date_t d = date();
                if( d == 0 ) s << '0';
                else
                    s << '"' << date().toString() << '"';
            }
            else
                s << date();
            if ( format == Strict )
                s << " }";
            else
                s << " )";
            break;
        case RegEx:
            if ( format == Strict ) {
                s << "{ \"$regex\" : \"" << escape( regex() );
                s << "\", \"$options\" : \"" << regexFlags() << "\" }";
            }
            else {
                s << "/" << escape( regex() , true ) << "/";
                // FIXME Worry about alpha order?
                for ( const char *f = regexFlags(); *f; ++f ) {
                    switch ( *f ) {
                    case 'g':
                    case 'i':
                    case 'm':
                        s << *f;
                    default:
                        break;
                    }
                }
            }
            break;

        case CodeWScope: {
            BSONObj scope = codeWScopeObject();
            if ( ! scope.isEmpty() ) {
                s << "{ \"$code\" : " << _asCode() << " , "
                  << " \"$scope\" : " << scope.jsonString() << " }";
                break;
            }
        }

        case Code:
            s << _asCode();
            break;

        case Timestamp:
            s << "{ \"t\" : " << timestampTime() << " , \"i\" : " << timestampInc() << " }";
            break;

        case MinKey:
            s << "{ \"$minKey\" : 1 }";
            break;

        case MaxKey:
            s << "{ \"$maxKey\" : 1 }";
            break;

        default:
            StringBuilder ss;
            ss << "Cannot create a properly formatted JSON string with "
               << "element: " << toString() << " of type: " << type();
            string message = ss.str();
            massert( 10312 ,  message.c_str(), false );
        }
        return s.str();
    }

    int BSONElement::getGtLtOp( int def ) const {
        const char *fn = fieldName();
        if ( fn[0] == '$' && fn[1] ) {
            if ( fn[2] == 't' ) {
                if ( fn[1] == 'g' ) {
                    if ( fn[3] == 0 ) return BSONObj::GT;
                    else if ( fn[3] == 'e' && fn[4] == 0 ) return BSONObj::GTE;
                }
                else if ( fn[1] == 'l' ) {
                    if ( fn[3] == 0 ) return BSONObj::LT;
                    else if ( fn[3] == 'e' && fn[4] == 0 ) return BSONObj::LTE;
                }
            }
            else if ( fn[1] == 'n' && fn[2] == 'e' ) {
                if ( fn[3] == 0 )
                    return BSONObj::NE;
                if ( fn[3] == 'a' && fn[4] == 'r') // matches anything with $near prefix
                    return BSONObj::opNEAR;
            }
            else if ( fn[1] == 'm' ) {
                if ( fn[2] == 'o' && fn[3] == 'd' && fn[4] == 0 )
                    return BSONObj::opMOD;
                if ( fn[2] == 'a' && fn[3] == 'x' && fn[4] == 'D' && fn[5] == 'i' && fn[6] == 's' && fn[7] == 't' && fn[8] == 'a' && fn[9] == 'n' && fn[10] == 'c' && fn[11] == 'e' && fn[12] == 0 )
                    return BSONObj::opMAX_DISTANCE;
            }
            else if ( fn[1] == 't' && fn[2] == 'y' && fn[3] == 'p' && fn[4] == 'e' && fn[5] == 0 )
                return BSONObj::opTYPE;
            else if ( fn[1] == 'i' && fn[2] == 'n' && fn[3] == 0 )
                return BSONObj::opIN;
            else if ( fn[1] == 'n' && fn[2] == 'i' && fn[3] == 'n' && fn[4] == 0 )
                return BSONObj::NIN;
            else if ( fn[1] == 'a' && fn[2] == 'l' && fn[3] == 'l' && fn[4] == 0 )
                return BSONObj::opALL;
            else if ( fn[1] == 's' && fn[2] == 'i' && fn[3] == 'z' && fn[4] == 'e' && fn[5] == 0 )
                return BSONObj::opSIZE;
            else if ( fn[1] == 'e' ) {
                if ( fn[2] == 'x' && fn[3] == 'i' && fn[4] == 's' && fn[5] == 't' && fn[6] == 's' && fn[7] == 0 )
                    return BSONObj::opEXISTS;
                if ( fn[2] == 'l' && fn[3] == 'e' && fn[4] == 'm' && fn[5] == 'M' && fn[6] == 'a' && fn[7] == 't' && fn[8] == 'c' && fn[9] == 'h' && fn[10] == 0 )
                    return BSONObj::opELEM_MATCH;
            }
            else if ( fn[1] == 'r' && fn[2] == 'e' && fn[3] == 'g' && fn[4] == 'e' && fn[5] == 'x' && fn[6] == 0 )
                return BSONObj::opREGEX;
            else if ( fn[1] == 'o' && fn[2] == 'p' && fn[3] == 't' && fn[4] == 'i' && fn[5] == 'o' && fn[6] == 'n' && fn[7] == 's' && fn[8] == 0 )
                return BSONObj::opOPTIONS;
            else if ( fn[1] == 'w' && fn[2] == 'i' && fn[3] == 't' && fn[4] == 'h' && fn[5] == 'i' && fn[6] == 'n' && fn[7] == 0 )
                return BSONObj::opWITHIN;
        }
        return def;
    }

    /* Matcher --------------------------------------*/

// If the element is something like:
//   a : { $gt : 3 }
// we append
//   a : 3
// else we just append the element.
//
    void appendElementHandlingGtLt(BSONObjBuilder& b, const BSONElement& e) {
        if ( e.type() == Object ) {
            BSONElement fe = e.embeddedObject().firstElement();
            const char *fn = fe.fieldName();
            if ( fn[0] == '$' && fn[1] && fn[2] == 't' ) {
                b.appendAs(fe, e.fieldName());
                return;
            }
        }
        b.append(e);
    }

    int getGtLtOp(const BSONElement& e) {
        if ( e.type() != Object )
            return BSONObj::Equality;

        BSONElement fe = e.embeddedObject().firstElement();
        return fe.getGtLtOp();
    }

    FieldCompareResult compareDottedFieldNames( const string& l , const string& r ) {
        static int maxLoops = 1024 * 1024;

        size_t lstart = 0;
        size_t rstart = 0;

        for ( int i=0; i<maxLoops; i++ ) {
            if ( lstart >= l.size() ) {
                if ( rstart >= r.size() )
                    return SAME;
                return RIGHT_SUBFIELD;
            }
            if ( rstart >= r.size() )
                return LEFT_SUBFIELD;

            size_t a = l.find( '.' , lstart );
            size_t b = r.find( '.' , rstart );

            size_t lend = a == string::npos ? l.size() : a;
            size_t rend = b == string::npos ? r.size() : b;

            const string& c = l.substr( lstart , lend - lstart );
            const string& d = r.substr( rstart , rend - rstart );

            int x = lexNumCmp( c.c_str(), d.c_str() );

            if ( x < 0 )
                return LEFT_BEFORE;
            if ( x > 0 )
                return RIGHT_BEFORE;

            lstart = lend + 1;
            rstart = rend + 1;
        }

        log() << "compareDottedFieldNames ERROR  l: " << l << " r: " << r << "  TOO MANY LOOPS" << endl;
        assert(0);
        return SAME; // will never get here
    }

    /* BSONObj ------------------------------------------------------------*/

    string BSONObj::md5() const {
        md5digest d;
        md5_state_t st;
        md5_init(&st);
        md5_append( &st , (const md5_byte_t*)_objdata , objsize() );
        md5_finish(&st, d);
        return digestToString( d );
    }

    string BSONObj::jsonString( JsonStringFormat format, int pretty ) const {

        if ( isEmpty() ) return "{}";

        StringBuilder s;
        s << "{ ";
        BSONObjIterator i(*this);
        BSONElement e = i.next();
        if ( !e.eoo() )
            while ( 1 ) {
                s << e.jsonString( format, true, pretty?pretty+1:0 );
                e = i.next();
                if ( e.eoo() )
                    break;
                s << ",";
                if ( pretty ) {
                    s << '\n';
                    for( int x = 0; x < pretty; x++ )
                        s << "  ";
                }
                else {
                    s << " ";
                }
            }
        s << " }";
        return s.str();
    }

    bool BSONObj::valid() const {
        try {
            BSONObjIterator it(*this);
            while( it.moreWithEOO() ) {
                // both throw exception on failure
                BSONElement e = it.next(true);
                e.validate();

                if (e.eoo()) {
                    if (it.moreWithEOO())
                        return false;
                    return true;
                }
                else if (e.isABSONObj()) {
                    if(!e.embeddedObject().valid())
                        return false;
                }
                else if (e.type() == CodeWScope) {
                    if(!e.codeWScopeObject().valid())
                        return false;
                }
            }
        }
        catch (...) {
        }
        return false;
    }

    int BSONObj::woCompare(const BSONObj& r, const Ordering &o, bool considerFieldName) const {
        if ( isEmpty() )
            return r.isEmpty() ? 0 : -1;
        if ( r.isEmpty() )
            return 1;

        BSONObjIterator i(*this);
        BSONObjIterator j(r);
        unsigned mask = 1;
        while ( 1 ) {
            // so far, equal...

            BSONElement l = i.next();
            BSONElement r = j.next();
            if ( l.eoo() )
                return r.eoo() ? 0 : -1;
            if ( r.eoo() )
                return 1;

            int x;
            {
                x = l.woCompare( r, considerFieldName );
                if( o.descending(mask) )
                    x = -x;
            }
            if ( x != 0 )
                return x;
            mask <<= 1;
        }
        return -1;
    }

    /* well ordered compare */
    int BSONObj::woCompare(const BSONObj &r, const BSONObj &idxKey,
                           bool considerFieldName) const {
        if ( isEmpty() )
            return r.isEmpty() ? 0 : -1;
        if ( r.isEmpty() )
            return 1;

        bool ordered = !idxKey.isEmpty();

        BSONObjIterator i(*this);
        BSONObjIterator j(r);
        BSONObjIterator k(idxKey);
        while ( 1 ) {
            // so far, equal...

            BSONElement l = i.next();
            BSONElement r = j.next();
            BSONElement o;
            if ( ordered )
                o = k.next();
            if ( l.eoo() )
                return r.eoo() ? 0 : -1;
            if ( r.eoo() )
                return 1;

            int x;
            /*
                        if( ordered && o.type() == String && strcmp(o.valuestr(), "ascii-proto") == 0 &&
                            l.type() == String && r.type() == String ) {
                            // note: no negative support yet, as this is just sort of a POC
                            x = _stricmp(l.valuestr(), r.valuestr());
                        }
                        else*/ {
                x = l.woCompare( r, considerFieldName );
                if ( ordered && o.number() < 0 )
                    x = -x;
            }
            if ( x != 0 )
                return x;
        }
        return -1;
    }

    BSONObj staticNull = fromjson( "{'':null}" );

    /* well ordered compare */
    int BSONObj::woSortOrder(const BSONObj& other, const BSONObj& sortKey , bool useDotted ) const {
        if ( isEmpty() )
            return other.isEmpty() ? 0 : -1;
        if ( other.isEmpty() )
            return 1;

        uassert( 10060 ,  "woSortOrder needs a non-empty sortKey" , ! sortKey.isEmpty() );

        BSONObjIterator i(sortKey);
        while ( 1 ) {
            BSONElement f = i.next();
            if ( f.eoo() )
                return 0;

            BSONElement l = useDotted ? getFieldDotted( f.fieldName() ) : getField( f.fieldName() );
            if ( l.eoo() )
                l = staticNull.firstElement();
            BSONElement r = useDotted ? other.getFieldDotted( f.fieldName() ) : other.getField( f.fieldName() );
            if ( r.eoo() )
                r = staticNull.firstElement();

            int x = l.woCompare( r, false );
            if ( f.number() < 0 )
                x = -x;
            if ( x != 0 )
                return x;
        }
        return -1;
    }

    template <typename BSONElementColl>
    void _getFieldsDotted( const BSONObj* obj, const StringData& name, BSONElementColl &ret, bool expandLastArray ) {
        BSONElement e = obj->getField( name );

        if ( e.eoo() ) {
            const char *p = strchr(name.data(), '.');
            if ( p ) {
                string left(name.data(), p-name.data());
                const char* next = p+1;
                BSONElement e = obj->getField( left.c_str() );

                if (e.type() == Object) {
                    e.embeddedObject().getFieldsDotted(next, ret, expandLastArray );
                }
                else if (e.type() == Array) {
                    bool allDigits = false;
                    if ( isdigit( *next ) ) {
                        const char * temp = next + 1;
                        while ( isdigit( *temp ) )
                            temp++;
                        allDigits = (*temp == '.' || *temp == '\0');
                    }
                    if (allDigits) {
                        e.embeddedObject().getFieldsDotted(next, ret, expandLastArray );
                    }
                    else {
                        BSONObjIterator i(e.embeddedObject());
                        while ( i.more() ) {
                            BSONElement e2 = i.next();
                            if (e2.type() == Object || e2.type() == Array)
                                e2.embeddedObject().getFieldsDotted(next, ret, expandLastArray );
                        }
                    }
                }
                else {
                    // do nothing: no match
                }
            }
        }
        else {
            if (e.type() == Array && expandLastArray) {
                BSONObjIterator i(e.embeddedObject());
                while ( i.more() )
                    ret.insert(i.next());
            }
            else {
                ret.insert(e);
            }
        }
    }

    void BSONObj::getFieldsDotted(const StringData& name, BSONElementSet &ret, bool expandLastArray ) const {
        _getFieldsDotted( this, name, ret, expandLastArray );
    }
    void BSONObj::getFieldsDotted(const StringData& name, BSONElementMSet &ret, bool expandLastArray ) const {
        _getFieldsDotted( this, name, ret, expandLastArray );
    }

    BSONElement BSONObj::getFieldDottedOrArray(const char *&name) const {
        const char *p = strchr(name, '.');

        BSONElement sub;

        if ( p ) {
            sub = getField( string(name, p-name) );
            name = p + 1;
        }
        else {
            sub = getField( name );
            name = name + strlen(name);
        }

        if ( sub.eoo() )
            return nullElement;
        else if ( sub.type() == Array || name[0] == '\0')
            return sub;
        else if ( sub.type() == Object )
            return sub.embeddedObject().getFieldDottedOrArray( name );
        else
            return nullElement;
    }

    /**
     sets element field names to empty string
     If a field in pattern is missing, it is omitted from the returned
     object.
     */
    BSONObj BSONObj::extractFieldsUnDotted(BSONObj pattern) const {
        BSONObjBuilder b;
        BSONObjIterator i(pattern);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            BSONElement x = getField(e.fieldName());
            if ( !x.eoo() )
                b.appendAs(x, "");
        }
        return b.obj();
    }

    BSONObj BSONObj::extractFields(const BSONObj& pattern , bool fillWithNull ) const {
        BSONObjBuilder b(32); // scanandorder.h can make a zillion of these, so we start the allocation very small
        BSONObjIterator i(pattern);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            BSONElement x = getFieldDotted(e.fieldName());
            if ( ! x.eoo() )
                b.appendAs( x, e.fieldName() );
            else if ( fillWithNull )
                b.appendNull( e.fieldName() );
        }
        return b.obj();
    }

    BSONObj BSONObj::filterFieldsUndotted( const BSONObj &filter, bool inFilter ) const {
        BSONObjBuilder b;
        BSONObjIterator i( *this );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            BSONElement x = filter.getField( e.fieldName() );
            if ( ( x.eoo() && !inFilter ) ||
                    ( !x.eoo() && inFilter ) )
                b.append( e );
        }
        return b.obj();
    }

    BSONElement BSONObj::getFieldUsingIndexNames(const char *fieldName, const BSONObj &indexKey) const {
        BSONObjIterator i( indexKey );
        int j = 0;
        while( i.moreWithEOO() ) {
            BSONElement f = i.next();
            if ( f.eoo() )
                return BSONElement();
            if ( strcmp( f.fieldName(), fieldName ) == 0 )
                break;
            ++j;
        }
        BSONObjIterator k( *this );
        while( k.moreWithEOO() ) {
            BSONElement g = k.next();
            if ( g.eoo() )
                return BSONElement();
            if ( j == 0 ) {
                return g;
            }
            --j;
        }
        return BSONElement();
    }

    /* grab names of all the fields in this object */
    int BSONObj::getFieldNames(set<string>& fields) const {
        int n = 0;
        BSONObjIterator i(*this);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            fields.insert(e.fieldName());
            n++;
        }
        return n;
    }

    /* note: addFields always adds _id even if not specified
       returns n added not counting _id unless requested.
    */
    int BSONObj::addFields(BSONObj& from, set<string>& fields) {
        assert( isEmpty() && !isOwned() ); /* partial implementation for now... */

        BSONObjBuilder b;

        int N = fields.size();
        int n = 0;
        BSONObjIterator i(from);
        bool gotId = false;
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            const char *fname = e.fieldName();
            if ( fields.count(fname) ) {
                b.append(e);
                ++n;
                gotId = gotId || strcmp(fname, "_id")==0;
                if ( n == N && gotId )
                    break;
            }
            else if ( strcmp(fname, "_id")==0 ) {
                b.append(e);
                gotId = true;
                if ( n == N && gotId )
                    break;
            }
        }

        if ( n ) {
            *this = b.obj();
        }

        return n;
    }

    BSONObj BSONObj::clientReadable() const {
        BSONObjBuilder b;
        BSONObjIterator i( *this );
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            switch( e.type() ) {
            case MinKey: {
                BSONObjBuilder m;
                m.append( "$minElement", 1 );
                b.append( e.fieldName(), m.done() );
                break;
            }
            case MaxKey: {
                BSONObjBuilder m;
                m.append( "$maxElement", 1 );
                b.append( e.fieldName(), m.done() );
                break;
            }
            default:
                b.append( e );
            }
        }
        return b.obj();
    }

    BSONObj BSONObj::replaceFieldNames( const BSONObj &names ) const {
        BSONObjBuilder b;
        BSONObjIterator i( *this );
        BSONObjIterator j( names );
        BSONElement f = j.moreWithEOO() ? j.next() : BSONObj().firstElement();
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;
            if ( !f.eoo() ) {
                b.appendAs( e, f.fieldName() );
                f = j.next();
            }
            else {
                b.append( e );
            }
        }
        return b.obj();
    }

    bool BSONObj::okForStorage() const {
        BSONObjIterator i( *this );
        while ( i.more() ) {
            BSONElement e = i.next();
            const char * name = e.fieldName();

            if ( strchr( name , '.' ) ||
                    strchr( name , '$' ) ) {
                return
                    strcmp( name , "$ref" ) == 0 ||
                    strcmp( name , "$id" ) == 0
                    ;
            }

            if ( e.mayEncapsulate() ) {
                switch ( e.type() ) {
                case Object:
                case Array:
                    if ( ! e.embeddedObject().okForStorage() )
                        return false;
                    break;
                case CodeWScope:
                    if ( ! e.codeWScopeObject().okForStorage() )
                        return false;
                    break;
                default:
                    uassert( 12579, "unhandled cases in BSONObj okForStorage" , 0 );
                }

            }
        }
        return true;
    }

    void BSONObj::dump() const {
        out() << hex;
        const char *p = objdata();
        for ( int i = 0; i < objsize(); i++ ) {
            out() << i << '\t' << ( 0xff & ( (unsigned) *p ) );
            if ( *p >= 'A' && *p <= 'z' )
                out() << '\t' << *p;
            out() << endl;
            p++;
        }
    }

    void nested2dotted(BSONObjBuilder& b, const BSONObj& obj, const string& base) {
        BSONObjIterator it(obj);
        while (it.more()) {
            BSONElement e = it.next();
            if (e.type() == Object) {
                string newbase = base + e.fieldName() + ".";
                nested2dotted(b, e.embeddedObject(), newbase);
            }
            else {
                string newbase = base + e.fieldName();
                b.appendAs(e, newbase);
            }
        }
    }

    void dotted2nested(BSONObjBuilder& b, const BSONObj& obj) {
        //use map to sort fields
        BSONMap sorted = bson2map(obj);
        EmbeddedBuilder eb(&b);
        for(BSONMap::const_iterator it=sorted.begin(); it!=sorted.end(); ++it) {
            eb.appendAs(it->second, it->first);
        }
        eb.done();
    }

    /*-- test things ----------------------------------------------------*/

#pragma pack(1)
    struct MaxKeyData {
        MaxKeyData() {
            totsize=7;
            maxkey=MaxKey;
            name=0;
            eoo=EOO;
        }
        int totsize;
        char maxkey;
        char name;
        char eoo;
    } maxkeydata;
    BSONObj maxKey((const char *) &maxkeydata);

    struct MinKeyData {
        MinKeyData() {
            totsize=7;
            minkey=MinKey;
            name=0;
            eoo=EOO;
        }
        int totsize;
        char minkey;
        char name;
        char eoo;
    } minkeydata;
    BSONObj minKey((const char *) &minkeydata);

    /*
        struct JSObj0 {
            JSObj0() {
                totsize = 5;
                eoo = EOO;
            }
            int totsize;
            char eoo;
        } js0;
    */
#pragma pack()

    struct BsonUnitTest : public UnitTest {
        void testRegex() {

            BSONObjBuilder b;
            b.appendRegex("x", "foo");
            BSONObj o = b.done();

            BSONObjBuilder c;
            c.appendRegex("x", "goo");
            BSONObj p = c.done();

            assert( !o.shallowEqual( p ) );
            assert( o.woCompare( p ) < 0 );

        }
        void testoid() {
            OID id;
            id.init();
            //            sleepsecs(3);

            OID b;
            // goes with sleep above...
            // b.init();
            // assert( memcmp(id.getData(), b.getData(), 12) < 0 );

            b.init( id.str() );
            assert( b == id );
        }

        void testbounds() {
            BSONObj l , r;
            {
                BSONObjBuilder b;
                b.append( "x" , numeric_limits<long long>::max() );
                l = b.obj();
            }
            {
                BSONObjBuilder b;
                b.append( "x" , numeric_limits<double>::max() );
                r = b.obj();
            }
            assert( l.woCompare( r ) < 0 );
            assert( r.woCompare( l ) > 0 );
            {
                BSONObjBuilder b;
                b.append( "x" , numeric_limits<int>::max() );
                l = b.obj();
            }
            assert( l.woCompare( r ) < 0 );
            assert( r.woCompare( l ) > 0 );
        }

        void testorder() {
            {
                BSONObj x,y,z;
                { BSONObjBuilder b; b.append( "x" , (long long)2 ); x = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (int)3 ); y = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (long long)4 ); z = b.obj(); }
                assert( x.woCompare( y ) < 0 );
                assert( x.woCompare( z ) < 0 );
                assert( y.woCompare( x ) > 0 );
                assert( z.woCompare( x ) > 0 );
                assert( y.woCompare( z ) < 0 );
                assert( z.woCompare( y ) > 0 );
            }

            {
                BSONObj ll,d,i,n,u;
                { BSONObjBuilder b; b.append( "x" , (long long)2 ); ll = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (double)2 ); d = b.obj(); }
                { BSONObjBuilder b; b.append( "x" , (int)2 ); i = b.obj(); }
                { BSONObjBuilder b; b.appendNull( "x" ); n = b.obj(); }
                { BSONObjBuilder b; u = b.obj(); }

                assert( ll.woCompare( u ) == d.woCompare( u ) );
                assert( ll.woCompare( u ) == i.woCompare( u ) );
                BSONObj k = BSON( "x" << 1 );
                assert( ll.woCompare( u , k ) == d.woCompare( u , k ) );
                assert( ll.woCompare( u , k ) == i.woCompare( u , k ) );

                assert( u.woCompare( ll ) == u.woCompare( d ) );
                assert( u.woCompare( ll ) == u.woCompare( i ) );
                assert( u.woCompare( ll , k ) == u.woCompare( d , k ) );
                assert( u.woCompare( ll , k ) == u.woCompare( d , k ) );

                assert( i.woCompare( n ) == d.woCompare( n ) );

                assert( ll.woCompare( n ) == d.woCompare( n ) );
                assert( ll.woCompare( n ) == i.woCompare( n ) );
                assert( ll.woCompare( n , k ) == d.woCompare( n , k ) );
                assert( ll.woCompare( n , k ) == i.woCompare( n , k ) );

                assert( n.woCompare( ll ) == n.woCompare( d ) );
                assert( n.woCompare( ll ) == n.woCompare( i ) );
                assert( n.woCompare( ll , k ) == n.woCompare( d , k ) );
                assert( n.woCompare( ll , k ) == n.woCompare( d , k ) );
            }

            {
                BSONObj l,r;
                { BSONObjBuilder b; b.append( "x" , "eliot" ); l = b.obj(); }
                { BSONObjBuilder b; b.appendSymbol( "x" , "eliot" ); r = b.obj(); }
                assert( l.woCompare( r ) == 0 );
                assert( r.woCompare( l ) == 0 );
            }
        }

        void run() {
            testRegex();
            BSONObjBuilder A,B,C;
            A.append("x", 2);
            B.append("x", 2.0);
            C.append("x", 2.1);
            BSONObj a = A.done();
            BSONObj b = B.done();
            BSONObj c = C.done();
            assert( !a.shallowEqual( b ) ); // comments on operator==
            int cmp = a.woCompare(b);
            assert( cmp == 0 );
            cmp = a.woCompare(c);
            assert( cmp < 0 );
            testoid();
            testbounds();
            testorder();
        }
    } bson_unittest;

    Labeler::Label GT( "$gt" );
    Labeler::Label GTE( "$gte" );
    Labeler::Label LT( "$lt" );
    Labeler::Label LTE( "$lte" );
    Labeler::Label NE( "$ne" );
    Labeler::Label SIZE( "$size" );

    void BSONObjBuilder::appendMinForType( const StringData& fieldName , int t ) {
        switch ( t ) {
                
        // Shared canonical types
        case NumberInt:
        case NumberDouble:
        case NumberLong:
            append( fieldName , - numeric_limits<double>::max() ); return;
        case Symbol:
        case String:
            append( fieldName , "" ); return;
        case Date: 
            // min varies with V0 and V1 indexes, so we go one type lower.
            appendBool(fieldName, true);
            //appendDate( fieldName , numeric_limits<long long>::min() ); 
            return;
        case Timestamp: // TODO integrate with Date SERVER-3304
            appendTimestamp( fieldName , 0 ); return;
        case Undefined: // shared with EOO
            appendUndefined( fieldName ); return;
                
        // Separate canonical types
        case MinKey:
            appendMinKey( fieldName ); return;
        case MaxKey:
            appendMaxKey( fieldName ); return;
        case jstOID: {
            OID o;
            memset(&o, 0, sizeof(o));
            appendOID( fieldName , &o);
            return;
        }
        case Bool:
            appendBool( fieldName , false); return;
        case jstNULL:
            appendNull( fieldName ); return;
        case Object:
            append( fieldName , BSONObj() ); return;
        case Array:
            appendArray( fieldName , BSONObj() ); return;
        case BinData:
            appendBinData( fieldName , 0 , BinDataGeneral , (const char *) 0 ); return;
        case RegEx:
            appendRegex( fieldName , "" ); return;
        case DBRef: {
            OID o;
            memset(&o, 0, sizeof(o));
            appendDBRef( fieldName , "" , o );
            return;
        }
        case Code:
            appendCode( fieldName , "" ); return;
        case CodeWScope:
            appendCodeWScope( fieldName , "" , BSONObj() ); return;
        };
        log() << "type not supported for appendMinElementForType: " << t << endl;
        uassert( 10061 ,  "type not supported for appendMinElementForType" , false );
    }

    void BSONObjBuilder::appendMaxForType( const StringData& fieldName , int t ) {
        switch ( t ) {
                
        // Shared canonical types
        case NumberInt:
        case NumberDouble:
        case NumberLong:
            append( fieldName , numeric_limits<double>::max() ); return;
        case Symbol:
        case String:
            appendMinForType( fieldName, Object ); return;
        case Date:
            appendDate( fieldName , numeric_limits<long long>::max() ); return;
        case Timestamp: // TODO integrate with Date SERVER-3304
            appendTimestamp( fieldName , numeric_limits<unsigned long long>::max() ); return;
        case Undefined: // shared with EOO
            appendUndefined( fieldName ); return;

        // Separate canonical types
        case MinKey:
            appendMinKey( fieldName ); return;
        case MaxKey:
            appendMaxKey( fieldName ); return;
        case jstOID: {
            OID o;
            memset(&o, 0xFF, sizeof(o));
            appendOID( fieldName , &o);
            return;
        }
        case Bool:
            appendBool( fieldName , true ); return;
        case jstNULL:
            appendNull( fieldName ); return;
        case Object:
            appendMinForType( fieldName, Array ); return;
        case Array:
            appendMinForType( fieldName, BinData ); return;
        case BinData:
            appendMinForType( fieldName, jstOID ); return;
        case RegEx:
            appendMinForType( fieldName, DBRef ); return;
        case DBRef:
            appendMinForType( fieldName, Code ); return;                
        case Code:
            appendMinForType( fieldName, CodeWScope ); return;
        case CodeWScope:
            // This upper bound may change if a new bson type is added.
            appendMinForType( fieldName , MaxKey ); return;
        }
        log() << "type not supported for appendMaxElementForType: " << t << endl;
        uassert( 14853 ,  "type not supported for appendMaxElementForType" , false );
    }

    int BSONElementFieldSorter( const void * a , const void * b ) {
        const char * x = *((const char**)a);
        const char * y = *((const char**)b);
        x++; y++;
        return lexNumCmp( x , y );
    }

    bool fieldsMatch(const BSONObj& lhs, const BSONObj& rhs) {
        BSONObjIterator l(lhs);
        BSONObjIterator r(rhs);

        while (l.more() && r.more()){
            if (strcmp(l.next().fieldName(), r.next().fieldName())) {
                PRINTFL;
                return false;
            }
        }
        PRINT(l.more());
        PRINT(r.more());
        PRINT(l.more() || r.more());

        return !(l.more() || r.more()); // false if lhs and rhs have diff nFields()
    }

    BSONObjIteratorSorted::BSONObjIteratorSorted( const BSONObj& o ) {
        _nfields = o.nFields();
        _fields = new const char*[_nfields];
        int x = 0;
        BSONObjIterator i( o );
        while ( i.more() ) {
            _fields[x++] = i.next().rawdata();
            assert( _fields[x-1] );
        }
        assert( x == _nfields );
        qsort( _fields , _nfields , sizeof(char*) , BSONElementFieldSorter );
        _cur = 0;
    }

    bool BSONObjBuilder::appendAsNumber( const StringData& fieldName , const string& data ) {
        if ( data.size() == 0 || data == "-" || data == ".")
            return false;

        unsigned int pos=0;
        if ( data[0] == '-' )
            pos++;

        bool hasDec = false;

        for ( ; pos<data.size(); pos++ ) {
            if ( isdigit(data[pos]) )
                continue;

            if ( data[pos] == '.' ) {
                if ( hasDec )
                    return false;
                hasDec = true;
                continue;
            }

            return false;
        }

        if ( hasDec ) {
            double d = atof( data.c_str() );
            append( fieldName , d );
            return true;
        }

        if ( data.size() < 8 ) {
            append( fieldName , atoi( data.c_str() ) );
            return true;
        }

        try {
            long long num = boost::lexical_cast<long long>( data );
            append( fieldName , num );
            return true;
        }
        catch(bad_lexical_cast &) {
            return false;
        }
    }

} // namespace mongo
