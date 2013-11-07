#include <iostream>
#include <limits>
#include <algorithm>
#include <iomanip>
#include <vector>

#include <QFile>
#include <QString>
#include <QTextStream>
#include <QFileInfo>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QTime>
#include <cassert>

#include "extractor.h"

using namespace std;

// this is used to sort the keys when writing header file
// returns float for easier management down the road... :)
static float keywordPriority( const QString & key)
{
    if( key == "SIMPLE") return 0;
    if( key == "BITPIX") return 1;
    if( key == "NAXIS") return 2;
    if( key == "NAXIS1") return 3;
    if( key == "NAXIS2") return 4;
    if( key == "NAXIS3") return 5;
    if( key == "NAXIS4") return 6;
    if( key == "NAXIS5") return 7;
    if( key == "END") return numeric_limits<float>::max();
    return 1000000;
}

// FitsLine represents a single entry in the Fits header (I think it's called a card... :)
struct FitsLine {
    FitsLine( const QString & rawLine ) {
        _raw = rawLine;
    }
    QString raw() { return _raw; }
    QString key() { QString k, v, c; parse( k, v, c); return k; }
    QString value() { QString k, v, c; parse( k, v, c); return v; }
    QString comment() { QString k, v, c; parse( k, v, c); return c; }
    // parse the line into key/value/comment
    void parse( QString & key, QString & value, QString & comment ) {
        // key is the first 8 characters (trimmed)
        key = _raw.left(8).trimmed();
        // by default, value & comment are empty
        value = comment = QString();
        // if there is no equal sign present, return the default values for value/comment, which is empty
        if( _raw.mid( 8, 2).trimmed() != "=") return;
        // find the start/end of the value
        //   start = first non-white character
        //   end   = last character of the value (if string, it's the closing quote, otherwise it's the last non-space
        int vStart = 10, vEnd = -1;
        while( _raw[vStart].isSpace()) { vStart ++; if( vStart >= 80) { vStart = -1; break; }}
        if( vStart == -1) // entire line is empty after the '='
            return;
        if( _raw[vStart] != '\'') { // it's an unquoted value
            // non-string value, find the end
            vEnd = _raw.indexOf( '/', vStart + 1); if( vEnd != -1) vEnd --; else vEnd = 79;
            //            vEnd = vStart + 1;
            //            while( ! _raw[vEnd].isSpace()) { if( vEnd >= 80) break; else vEnd ++; }
            //            vEnd --;
        } else { // it's s quoted string
            // temporarily remove all occurrences of double single-quotes and then find the next single quote
            QString tmp = _raw; for(int i=0;i<=vStart;i++){tmp[i]=' ';} tmp.replace( "''", "..");
            vEnd = tmp.indexOf( '\'', vStart + 1);
            if( vEnd == -1) // we have an unterminated string here
                throw QString( "Unterminated string in header for %1").arg(key);
        }
        // now that we know start/end, get the value
        value = _raw.mid( vStart, vEnd - vStart + 1).trimmed();

        // if this was a string value, get rid of the double single-quotes permanently, and remove the surrounding quotes too
        //if( value[0] == '\'') value = value.mid( 1, value.length()-2).replace( "''", "'");

        // is there a comment?
        comment = _raw.mid( vEnd + 1).trimmed();
        if( ! comment.isEmpty()) {
            if( comment[0] != '/')
                throw ("Syntax error in header: " + _raw.trimmed());
            else
                comment.remove(0,1);
        }
    }


protected:
    QString _raw;
};

// represents a FITS header
struct FitsHeader
{
    // do the parse of the fits file
    static FitsHeader parse( QFile & f );
    // write the header to a file
    bool write( QFile & f);
    // was the parse successful?
    bool isValid() const { return _valid; }
    // find a line with a given key
    int findLine( const QString & key ) {
        for( size_t i = 0 ; i < _lines.size() ; i ++ )
            if( _lines[i].key() == key )
                return i;
        return -1;
    }
    qint64 dataOffset() const { return _dataOffset; }
    std::vector< FitsLine > & lines() { return _lines; }

    // add a raw line to the header
    void addRaw( const QString & line );

    // sets a value in the header
    void setIntValue( const QString & key, int value, const QString & comment = QString());
    void setDoubleValue(const QString & pkey, double value, const QString & pcomment = QString());

    // general access function to key/values, does not throw exceptions but can return
    // variant with isValid() = false
    QVariant getValue( const QString & key, QVariant defaultValue = QVariant());

    // convenience functions that lookup key and convert it to requested type
    // all these throw exceptions if (a) key is not defined (b) key does not have a value
    // that can be converted to the requested type:

    // find a line with 'key' and conver it's 'value' to integer
    int intValue( const QString & key );
    int intValue( const QString & key, int defaultValue);
    QString stringValue( const QString & key );
    QString stringValue( const QString & key, const QString & defaultValue);
    double doubleValue( const QString & key );
    double doubleValue( const QString & key, double defaultValue);


protected:
    // where does the data start? This is set only in parse()! Bad design, I know.
    qint64 _dataOffset;
    // is this header valid? This is also only set in parse();
    bool _valid;
    // the lines
    std::vector< FitsLine > _lines;
    // protected constructor
    FitsHeader() { _valid = false; _dataOffset = 0; }
    // convenienty 80 spaces string
    static QString space80;
};

QString FitsHeader::space80 = "                                                                                ";

// convenience function to convert fits string to a raw string (removing intial quotes & replacing all double quotes with single ones)
static QString fitsString2raw( const QString & s)
{
    if( s.length() < 2) throw "fitsString2raw - string less than 2 characters.";
    QString res = s;
    // remove the leading and ending quotes
    res[0] = res[ res.length()-1] = ' ';
    // replace all double single-quotes with a single quote
    res.replace( "''", "'");

    return res;
}

// remove leading/trailing spaces from a fits string
static QString fitsStringTrimmed( const QString & s)
{
    return QString( "'%1'").arg(fitsString2raw(s).trimmed());
}

// wrapper around regular QFile::read() - it makes sure to read in requested size 's' if possible
bool blockRead( QFile & f, char * ptr, qint64 s)
{
    qint64 remaining = s;
    while( remaining > 0 ) {
        qint64 d = f.read( (char *) ptr, remaining);
        if( d <= 0 ) {
            cerr << "Error: blockRead(): could not read another block.\n";
            return false;
        }
        // update remaining & ptr
        ptr += d;
        remaining -= d;
    }
    return true;
}

// wrapper around regular QFile::write() - it makes sure to write in requested size 's' if possible
bool blockWrite( QFile & f, const char * ptr, qint64 s)
{
    qint64 remaining = s;
    while( remaining > 0 ) {
        qint64 d = f.write( (const char *) ptr, remaining);
        if( d <= 0 ) {
            cerr << "Error: blockWrite(): could not write another block\n";
            return false;
        }
        // update remaining & ptr
        ptr += d;
        remaining -= d;
    }
    return true;
}

// fits header parser
FitsHeader FitsHeader::parse( QFile & f)
{
    FitsHeader hdr;

    // read in header one 'line' (or card) at a time, which is 80 bytes
    // until we find one that conains END
    while( 1)
    {
        // read in another header block
        char block[80];
        if( ! blockRead( f, block, 80)) {
            cerr << "Error: FitsHeader::parse() could not read card.\n";
            return hdr;
        }

        // clean up the block by converting anything outside of ASCII [32..126]
        // to spaces
        for( size_t i = 0 ; i < sizeof( block) ; i ++ )
            if( block[i] < 32 || block[i] > 126)
                block[i] = ' ';

        // data offset moves
        hdr._dataOffset += sizeof( block);

        // parse the block: one line at a time (there are 36 lines of 80 chars each,
        // but they are not \0 terminated!!!)
        QString rawLine = QByteArray( (char *) block, sizeof( block) );
        // add this line to the header
        hdr._lines.push_back( rawLine);
        // if this is the 'END' line, terminate the parse
        if( rawLine.startsWith( "END     " ))
            break;
    }
    // adjust offset to be a multiple of 2880
    hdr._dataOffset = ((hdr._dataOffset -1)/ 2880 + 1) * 2880;
    // return this header
    hdr._valid = true;
    return hdr;
}

// will write out the header to a file
// after sorting the lines by keyword priority and if keyword priority is the same then by
// the current line position
bool FitsHeader::write(QFile & f)
{
    // sort the lines based on a) keword priority, b) their current order
    //vector< pair< QString, pair< double, int> > > lines;
    typedef pair<QString, pair<double,int> > SortLine;
    vector<SortLine> lines;
    for( size_t i = 0 ; i < _lines.size() ; i ++ )
        lines.push_back( make_pair(_lines[i].raw(), make_pair( keywordPriority( _lines[i].key()), i)));
    // c++ does not support anonymous functions, but it does support local structures/classes with
    // static functions... go figure :)
    struct local { static bool cmp( const SortLine & v1, const SortLine & v2 ) {
            // use std::pair built in comparison, it compares first to first, and only if equal
            // it compares second to second
            return( v1.second < v2.second );
        }};
    std::sort( lines.begin(), lines.end(), local::cmp);
    // put all strings into one big array of bytes for wrting
    QByteArray block;
    for( size_t i = 0 ; i < lines.size() ; i ++ )
        block.append( (lines[i].first + space80).left(80)); // paranoia
    // pad with spaces so that the block is a multiple of 2880 bytes
    while( block.size() % 2880 )
        block.append( ' ');
    //    cerr << "FitsHeader::write() block size = " << block.size() << " with " << lines.size() << " lines\n";
    //    for( size_t i = 0 ; i < lines.size() ; i ++ )
    //        cerr << lines[i].first.toStdString() << "\n";
    if( ! blockWrite( f, block.constData(), block.size()))
        return false;
    else
        return true;
}

// get a value from the header as int - throwing an exception if this fails!
int FitsHeader::intValue( const QString & key)
{
    QVariant value = getValue( key);
    if( ! value.isValid())
        throw QString("Could not find key %1 in fits file.").arg(key);
    bool ok;
    int result = value.toInt( & ok);
    if( ! ok )
        throw QString("Found %1=%2 in fits file but expected an integer.").arg(key).arg(value.toString());

    // value converted, return it
    return result;
}

// get a value from the header as int - throwing an exception if this fails!
int FitsHeader::intValue( const QString & key, int defaultValue)
{
    QVariant value = getValue( key);
    if( ! value.isValid())
        return defaultValue;
    bool ok;
    int result = value.toInt( & ok);
    if( ! ok )
        throw QString("Found %1=%2 in fits file but expected an integer.").arg(key).arg(value.toString());

    // value converted, return it
    return result;
}


// get a value from the header as double - throwing an exception if this fails!
double FitsHeader::doubleValue( const QString & key)
{
    QVariant value = getValue( key);
    if( ! value.isValid())
        throw QString("Could not find key %1 in fits file.").arg(key);
    bool ok;
    double result = value.toDouble( & ok);
    if( ! ok )
        throw QString("Found %1=%2 in fits file but expected a double.").arg(key).arg(value.toString());

    // value converted, return it
    return result;
}

// get a value from the header as double - substituting default value if needed!
double FitsHeader::doubleValue( const QString & key, double defaultValue)
{
    QVariant value = getValue( key);
    if( ! value.isValid())
        return defaultValue;
    bool ok;
    double result = value.toDouble( & ok);
    if( ! ok )
        throw QString("Found %1=%2 in fits file but expected a double.").arg(key).arg(value.toString());

    // value converted, return it
    return result;
}


// get a value from the header as string - throwing an exception if this fails!
QString FitsHeader::stringValue( const QString & key)
{
    QVariant value = getValue( key);
    if( ! value.isValid())
        throw QString("Could not find key %1 in fits file.").arg(key);
    return value.toString();
}

// get a value from the header as string - throwing an exception if this fails!
QString FitsHeader::stringValue( const QString & key, const QString & defaultValue)
{
    QVariant value = getValue( key);
    if( ! value.isValid())
        return defaultValue;
    return value.toString();
}


// get a value from the header as int
QVariant FitsHeader::getValue( const QString & key, QVariant defaultValue)
{
    // find the line with this key
    int ind = findLine( key);

    // if there is no such line, report error
    if( ind < 0 )
        return defaultValue;

    // return the value as qvariant
    return QVariant( _lines[ind].value());
}

// set an integer value
void FitsHeader::setIntValue(const QString & pkey, int value, const QString & pcomment)
{
    QString key = (pkey + space80).left(8);
    QString comment = (pcomment + space80).left( 47);
    // construct a line based on the parameters
    QString rawLine = QString( "%1= %2 /  %3").arg( key, -8).arg( value, 20).arg( comment);
    rawLine = (rawLine + space80).left(80); // just in case :)
    // find a line with this key so that we can decide if we are adding a new line or
    // replacing an existing one
    int ind = findLine( pkey);
    if( ind < 0 )
        _lines.push_back( rawLine);
    else
        _lines[ind] = rawLine;
}

// set a double value
void FitsHeader::setDoubleValue(const QString & pkey, double value, const QString & pcomment)
{
    QString space80 = "                                                                                ";
    QString key = (pkey + space80).left(8);
    QString comment = (pcomment + space80).left( 47);
    // construct a line based on the parameters
    QString rawLine = QString( "%1= %2 /  %3").arg( key, -8).arg( value, 20, 'G', 10).arg( comment);
    rawLine = (rawLine + space80).left(80); // just in case :)
    // find a line with this key so that we can decide if we are adding a new line or
    // replacing an existing one
    int ind = findLine( pkey);
    if( ind < 0 )
        _lines.push_back( rawLine);
    else
        _lines[ind] = rawLine;
}

// insert a raw line into fits - no syntax checking is done, except making sure it's padded to 80 chars
void FitsHeader::addRaw(const QString & line)
{
    _lines.push_back( (line + space80).left(80));
}

// simple 3D array
template <class T> struct M3D {
    M3D( int dx, int dy, int dz ) {
        _raw.fill( 0, dx * dy * dz * sizeof(T));
        // cerr << "M3D allocated " << _raw.size() << " bytes\n";
        _dx = dx; _dy = dy; _dz = dz;
    }
    char * raw() { return _raw.data(); }
    T & operator()(int x, int y, int z) {
        qint64 ind = z; ind = ind * _dy + y; ind = ind * _dx + x; ind *= sizeof(T);
        return * (T *) (_raw.constData() + ind);
    }
    void reset() { _raw.fill(0); }
    //    void reset() { memset( (void *) _raw.constData(), 0, _raw.size()); }

    QByteArray _raw;
    int _dx, _dy, _dz;
};

// simple 2D array
template <class T> struct M2D {
    M2D( int dx, int dy ) {
        _raw.fill( 0, dx * dy * sizeof(T));
        // cerr << "M2D allocated " << _raw.size() << " bytes\n";
        _dx = dx; _dy = dy;
    }
    char * raw() { return _raw.data(); }
    T & operator()(int x, int y) {
        qint64 ind = (y * _dx + x) * sizeof(T);
        if( ind < 0 || ind >= _raw.size()) throw "M2D out of bounds";
        return * (T *) (_raw.constData() + ind);
    }
    void reset() { _raw.fill(0); }

    QByteArray _raw;
    int _dx, _dy;
};

// buffered 3D FITS cube accessor (by index x,y,z), specific to BITPIX = -32 (i.e. floats)
// almost acts as a 3D matrix but the data is stored on the disk
struct M3DFloatFile{
    M3DFloatFile( QFile * fp, qint64 offset, int dx, int dy, int dz) {
        _fp = fp; _offset = offset; _dx = dx; _dy = dy; _dz = dz;
        _buffStart = _buffEnd = -1;
        _buffSize = 4096;
        _buff = new char[ _buffSize];
        if( ! _buff) throw QString( "Could not allocate buffer");
    }
    ~M3DFloatFile() { delete[] _buff; }
    float operator()(int x, int y, int z) {
        qint64 ind = z; ind = ind * _dy + y; ind = ind * _dx + x; ind *= 4; ind += _offset;
        // if buffer does not yet point to what we need, read in stuff into buffer
        if( ind < _buffStart || ind + 3 > _buffEnd ) {
            qint64 req = _buffSize;
            if( ! _fp-> seek( ind)) throw QString( "Failed to seek to extract a float");
            qint64 len = _fp-> read( _buff, req);
            // cerr << "Reading " << ind << " + " << req << " --> " << len << "\n";
            if( len < 4) throw QString( "Failed to read");
            _buffStart = ind;
            _buffEnd = _buffStart + len - 1;
        }
        char * p = _buff + (ind - _buffStart);
        float val; uchar * d = (uchar *) (& val);
        d[0] = p[3]; d[1] = p[2]; d[2] = p[1]; d[3] = p[0];
        return val;
    }

    QFile * _fp; qint64 _offset; int _dx, _dy, _dz;
    char * _buff;
    qint64 _buffStart, _buffEnd, _buffSize;
};

// given a raw bitpix value (as found in FITS files), returns the number of bytes
// which is basically abs(bitpix)/8
static int bitpixToSize( int bitpix )
{
    if( bitpix == 8 ) return 1;
    if( bitpix == 16 ) return 2;
    if( bitpix == 32 ) return 4;
    if( bitpix == -32 ) return 4;
    if( bitpix == -64 ) return 8;

    throw QString( "Illegal value BITPIX = %1").arg( bitpix);
}


// values extracted from the fits header
struct FitsInfo {
    int bitpix;
    int naxis;
    int naxis1, naxis2, naxis3;
    double bscale, bzero;
    double crpix1, crpix2, crpix3, crval1, crval2, crval3, cdelt1, cdelt2, cdelt3;
    QString ctype1, ctype2, ctype3, cunit3, bunit;
    double equinox;
    int blank; bool hasBlank;
    qint64 dataOffset, dataSize;
    QString fileName;
    double frameStart; // frequency of the first frame
    double frameEnd; // frequency of the last frame
    double frameNext; // frequence of the next frame if there should be one
};

// gets parsed (relevant information about a FITS file)
FitsInfo parse( const QString & fname)
{
    //    cerr << QString( "parsing header from %1\n").arg( fname).toStdString();
    // open raw file for reading
    QFile fp( fname);
    if( ! fp.open( QFile::ReadOnly))
        throw QString( "Could not open FITS file for reading: %1").arg( fname);
    // read in the header
    FitsHeader hdr = FitsHeader::parse(fp);
    if( ! hdr.isValid())
        throw QString( "Could not parse FITS header from: %1").arg( fname);

    // extract some parameters from the fits file and also validate it a bit
    FitsInfo fits;
    fits.fileName = fname;
    if( hdr.stringValue("SIMPLE") != "T" )
        throw QString("FITS file does not have 'SIMPLE = T': %1").arg( fname);
    fits.bitpix = hdr.intValue( "BITPIX");
    (void) bitpixToSize( fits.bitpix); // throw an exception if BITPIX is invalid
    fits.naxis = hdr.intValue( "NAXIS");
    if( fits.naxis != 3)
        throw QString( "Cannot deal with files that have NAXIS=%1: %2").arg(fits.naxis).arg(fname);
    fits.naxis1 = hdr.intValue( "NAXIS1");
    fits.naxis2 = hdr.intValue( "NAXIS2");
    fits.naxis3 = hdr.intValue( "NAXIS3", 1);

    QVariant blank = hdr.getValue( "BLANK");
    if( blank.isValid()) {
        fits.blank = hdr.intValue( "BLANK");
        fits.hasBlank = true;
        // blank is only supported for BITPIX > 0
        if( fits.bitpix < 0)
            throw QString( "Invalid use of BLANK = %1 keyword with BITPIX = %2: %3").arg( fits.blank).arg( fits.bitpix).arg(fname);
    } else {
        fits.hasBlank = false;
    }

    fits.bzero = hdr.doubleValue( "BZERO", 0);
    fits.bscale = hdr.doubleValue( "BSCALE", 1);
    fits.crval1 = hdr.doubleValue( "CRVAL1", 0);
    fits.crval2 = hdr.doubleValue( "CRVAL2", 0);
    fits.crval3 = hdr.doubleValue( "CRVAL3", 0);
    // HACK for Sukhpreet's files
    // fits.crval3 *= 1000;
    fits.cdelt1 = hdr.doubleValue( "CDELT1", 1);
    fits.cdelt2 = hdr.doubleValue( "CDELT2", 1);
    fits.cdelt3 = hdr.doubleValue( "CDELT3", 1);
    fits.crpix1 = hdr.doubleValue( "CRPIX1", 0);
    fits.crpix2 = hdr.doubleValue( "CRPIX2", 0);
    fits.crpix3 = hdr.doubleValue( "CRPIX3", 0);
    fits.ctype1 = hdr.stringValue( "CTYPE1", "''");
    fits.ctype2 = hdr.stringValue( "CTYPE2", "''");
    fits.ctype3 = hdr.stringValue( "CTYPE3", "''");
    fits.cunit3 = hdr.stringValue( "CUNIT3", "''");
    fits.bunit = hdr.stringValue( "BUNIT", "''"); fits.bunit = fitsStringTrimmed( fits.bunit);
    fits.equinox = hdr.doubleValue( "EQUINOX", 2000.0);

    // make sure the data segment following header is big enough for the data
    qint64 inputSize = fp.size();
    fits.dataSize = qint64(fits.naxis1) * fits.naxis2 * fits.naxis3 * bitpixToSize( fits.bitpix);
    fits.dataOffset = hdr.dataOffset();
    if( fits.dataOffset + fits.dataSize > inputSize)
        throw QString( "Invalid fits file size. Maybe accidentally truncated?");
    // position the input to the offset
    if( ! fp.seek( fits.dataOffset))
        throw QString( "Could not read the data (seek failed)");
    fp.close();

    // figure out starting value of the frame
    fits.frameStart = (1-fits.crpix3) * fits.cdelt3 + fits.crval3;
    fits.frameEnd = fits.frameStart + (fits.naxis3-1) * fits.cdelt3;
    fits.frameNext = fits.frameStart + fits.naxis3 * fits.cdelt3;
    return fits;
}


// buffered 3D FITS cube accessor (by index x,y,z)
// almost acts as a 3D matrix but the data is stored on the disk
struct M3DBitpixFile {
    M3DBitpixFile( FitsInfo fits, QFile * fp, qint64 offset, int dx, int dy, int dz) {
        _fp = fp; _offset = offset; _dx = dx; _dy = dy; _dz = dz; _fits = fits;
        _dataSize = bitpixToSize( _fits.bitpix);
        _buffStart = _buffEnd = -1; // indicate invalid buffer
        _buffSize = 4096;
        _buff = new char[ _buffSize];
        if( ! _buff) throw QString( "Could not allocate buffer");
    }
    ~M3DBitpixFile() { delete[] _buff; }
    double operator()(int x, int y, int z) {
        qint64 ind = z; ind = ind * _dy + y; ind = ind * _dx + x; ind *= _dataSize; ind += _offset;
        // if buffer does not yet point to what we need, read in stuff into buffer
        if( ind < _buffStart || ind + _dataSize -1 > _buffEnd ) {
            qint64 req = _buffSize;
            if( ! _fp-> seek( ind)) throw QString( "Failed to seek to extract data");
            qint64 len = _fp-> read( _buff, req);
            if( len < _dataSize) {
                throw QString( "failed to read the minimum (%1").arg( _dataSize);
            }
            _buffStart = ind;
            _buffEnd = _buffStart + len - 1;
        }
        char * p = _buff + (ind - _buffStart);
        // put the endian-adjusted bytes to 'tmp'
        char tmpBuff[8];
        std::reverse_copy( p, p + _dataSize, tmpBuff);

        // convert the raw bytes to an initial double
        double original; char * tmp = tmpBuff;
        switch( _fits.bitpix) {
        case   8: original = double( * (quint8 *) tmp); break;
        case  16: original = double( * (qint16 *) tmp); break;
        case  32: original = double( * (qint32 *) tmp); break;
        case  64: original = double( * (qint64 *) tmp); break;
        case -32: original = double( * ( float *) tmp); break;
        case -64: original = double( * (double *) tmp); break;
        default: throw QString( "Illegal value BITPIX = %1").arg( _fits.bitpix);
        }

        // check to see if we should apply BLANK
        if( _fits.hasBlank && original == _fits.bzero + _fits.blank * _fits.bscale)
            return std::numeric_limits<double>::quiet_NaN();
        else
            return _fits.bzero + _fits.bscale * original;
    }

    QFile * _fp; qint64 _offset; int _dx, _dy, _dz, _dataSize;
    char * _buff;
    qint64 _buffStart, _buffEnd, _buffSize;
    FitsInfo _fits;

    //    double convertRawDataToDouble( char * p)
    //    {
    //        double original;
    //        switch( _fits.bitpix) {
    //        case   8: original = double( * (quint8 *) p); break;
    //        case  16: original = double( * (qint16 *) p); break;
    //        case  32: original = double( * (qint32 *) p); break;
    //        case  64: original = double( * (qint64 *) p); break;
    //        case -32: original = double( * ( float *) p); break;
    //        case -64: original = double( * (double *) p); break;
    //        default: throw QString( "Illegal value BITPIX = %1").arg( _fits.bitpix);
    //        }
    //        // check to see if we should apply BLANK
    //        if( _fits.hasBlank && original == _fits.bzero + _fits.blank * _fits.bscale)
    //            return std::numeric_limits<double>::quiet_NaN();
    //        else
    //            return _fits.bzero + _fits.bscale * original;
    //    }

};

QString formatBytes( qint64 size)
{
    double s = size;
    if( s < 1024) return QString("%1B").arg( s);
    s /= 1024;
    if( s < 1024) return QString("%1kB").arg( s, 0, 'f', 2);
    s /= 1024;
    if( s < 1024) return QString("%1MB").arg( s, 0, 'f', 2);
    s /= 1024;
    if( s < 1024) return QString("%1GB").arg( s, 0, 'f', 2);
    s /= 1024;
    return QString("%1TB").arg( s, 0, 'g', 2);
}

QString formatSeconds( double s)
{
    QString res;
    res += QString("%1h").arg(int( s / 3600));
    s -= int(s/3600) * 3600;
    res += QString("%1m").arg(int( s / 60), 2, 10, QChar('0'));
    s -= int(s/60) * 60;
    res += QString("%1s").arg(int( s / 1), 2, 10, QChar('0'));
    return res;

}

struct FitsInfoLess {
    bool operator()( const FitsInfo & f1, const FitsInfo & f2) {
        return f1.frameStart < f2.frameStart;
    }
};

struct FitsInfoGreater {
    bool operator()( const FitsInfo & f1, const FitsInfo & f2) {
        return f1.frameStart > f2.frameStart;
    }
};

static void checkForCompatibility( vector<FitsInfo> & fileInfo)
{
    FitsInfo & f1 = fileInfo[0];
    bool errors = false;
    for( size_t i = 1 ; i < fileInfo.size() ; i ++ ) {
        FitsInfo & f2 = fileInfo[i];
        string finfo = QString( "\n  %1\n  %2\n").arg(f1.fileName).arg(f2.fileName).toStdString();
        if( f1.bitpix != f2.bitpix) {
            cerr << "*** ERROR *** BITPIX incompatible between files:" << finfo; errors = true;
        }
        if( f1.naxis != f2.naxis) {
            cerr << "*** ERROR *** NAXIS incompatible between files:" << finfo; errors = true;
        }
        if( f1.naxis1 != f2.naxis1) {
            cerr << "*** ERROR *** NAXIS1 incompatible between files:" << finfo; errors = true;
        }
        if( f1.naxis2 != f2.naxis2) {
            cerr << "*** ERROR *** NAXIS2 incompatible between files:" << finfo; errors = true;
        }
        if( f1.bscale != f2.bscale) {
            cerr << "*** ERROR *** BSCALE incompatible between files:" << finfo; errors = true;
        }
        if( f1.bzero != f2.bzero) {
            cerr << "*** ERROR *** BZERO incompatible between files:" << finfo; errors = true;
        }
        if( f1.crpix1 != f2.crpix1) {
            cerr << "*** ERROR *** CRPIX1 incompatible between files:" << finfo; errors = true;
        }
        if( f1.crpix2 != f2.crpix2) {
            cerr << "*** ERROR *** CRPIX2 incompatible between files:" << finfo; errors = true;
        }
        if( f1.crval1 != f2.crval1) {
            cerr << "*** ERROR *** CRVAL1 incompatible between files:" << finfo; errors = true;
        }
        if( f1.crval2 != f2.crval2) {
            cerr << "*** ERROR *** CRVAL2 incompatible between files:" << finfo; errors = true;
        }
        if( f1.cdelt1 != f2.cdelt1) {
            cerr << "*** ERROR *** CDELT1 incompatible between files:" << finfo; errors = true;
        }
        if( f1.cdelt2 != f2.cdelt2) {
            cerr << "*** ERROR *** CDELT2 incompatible between files:" << finfo; errors = true;
        }
        if( f1.cdelt3 != f2.cdelt3) {
            cerr << "*** ERROR *** CDELT3 incompatible between files:" << finfo; errors = true;
        }
    }
    if( errors) throw "Incompatible FITS files.";

    // now check if they cover a consecutive range in the 3rd axis
//    double currStart = f1.frameStart;
    double currEnd = f1.frameStart + f1.cdelt3 * f1.naxis3;
    for( size_t i = 1 ; i < fileInfo.size() ; i ++) {
        FitsInfo & f1 = fileInfo[i-1];
        FitsInfo & f2 = fileInfo[i];
        double diff = f2.frameStart - currEnd;
        if( f1.cdelt3 < 0) diff = - diff;
//        cerr << QString("diff = %1 (%2..%3)\n").arg(diff,20,'f').arg(f2.frameStart,20,'f').arg(currEnd,20,'f').toStdString();
        if( diff / fabs(f1.cdelt3) > fabs( f1.cdelt3 / 1e6)) {
            cerr << "*** WARNING *** big gap (" << diff << ") between "
                 << QFileInfo(f2.fileName).fileName().toStdString() << " and "
                 << QFileInfo(f2.fileName).fileName().toStdString() << "\n";
        }
        if( diff / fabs(f1.cdelt3) < -fabs( f1.cdelt3 / 1e6)) {
            cerr << "*** WARNING *** big overlap (" << diff << ") between "
                 << QFileInfo(f2.fileName).fileName().toStdString() << " and "
                 << QFileInfo(f2.fileName).fileName().toStdString() << "\n";
        }
//        currStart = f2.frameStart;
        currEnd = f2.frameStart + f2.cdelt3 * f2.naxis3;
    }
}

void clipData( char * buff, qint64 n, double min, double max, FitsInfo & info) {
    if( info.bitpix != -32) {
        static bool once = false;
        if( ! once) {
            once = true;
            std::cerr << "Cannot apply data clipping to BITPIX = " << info.bitpix << "\n";
            return;
        }
    }
    if( n % sizeof(float)) throw "Data chunk not size of float...grrr";
    for( int i = 0 ; i < n ; i += sizeof(float)) {
        char * valPtr = buff + i;
        float val;
        // endian fix
        char * p = (char *) (& val);
        p[0] = valPtr[3];
        p[1] = valPtr[2];
        p[2] = valPtr[1];
        p[3] = valPtr[0];
        if( val < min || val > max)
            val = std::numeric_limits<float>::quiet_NaN();
        // restore endian
        valPtr[3] = p[0];
        valPtr[2] = p[1];
        valPtr[1] = p[2];
        valPtr[0] = p[3];
    }
}

// combine cubes into one
void combineFITS( const QStringList & inputFilenames, const QString & outputFileName )
{
    // parse all headers from the files info FitsInfo structures
    cerr << "Parsing all headers:\n";
    int combinedNaxis3 = 0;
    vector<FitsInfo> fileInfo;
    {
        for( int i = 0 ; i < inputFilenames.size() ; i ++ ) {
            FitsInfo fits = parse( inputFilenames[i]);
            fileInfo.push_back( fits);
            combinedNaxis3 += fits.naxis3;
            cerr << QString("  %1 freq: %2..%3,%4\n")
                    .arg(QFileInfo(fits.fileName).fileName())
                    .arg(fits.frameStart,0,'f')
                    .arg(fits.frameEnd,0,'f')
                    .arg(fits.frameNext,0,'f')
                    .toStdString();
        }
    }
    cerr << "Found " << combinedNaxis3 << " frames.\n";

    // sort the files based on frequency
    {
        FitsInfo fits = parse( fileInfo[0].fileName);
        if( fits.cdelt3 < 0) {
            cerr << "Sorting by freq. in descending order:\n";
            std::sort( fileInfo.begin(), fileInfo.end(), FitsInfoGreater());
        } else {
            cerr << "Sorting by freq. in ascending order:\n";
            std::sort( fileInfo.begin(), fileInfo.end(), FitsInfoLess());
        }
        for( size_t i = 0 ; i < fileInfo.size() ; i ++ )
            cerr << QString("  %1").arg(fileInfo[i].fileName).toStdString() << "\n";
    }

    // make sure fits headers are compatible
    std::cerr << "Checking for compatibility\n";
    checkForCompatibility( fileInfo);

    // start writing the output
    QFile ofp( outputFileName);
    if( ! ofp.open( QFile::WriteOnly | QFile::Truncate))
        throw QString( "Cannot open %1 for writing.").arg( outputFileName);

    // parse the header of the first file
    QFile fp1( fileInfo[0].fileName); if( ! fp1.open( QFile::ReadOnly)) throw QString("Cannot re-open %1").arg(inputFilenames[0]);
    FitsHeader hdr1 = FitsHeader::parse( fp1);
    fp1.close();

    // prepare the output header - by copying the original header
    FitsHeader outHeader = hdr1;
    outHeader.setIntValue( "NAXIS3", combinedNaxis3);
    // HACK for Sukhpreet's files
    // outHeader.setDoubleValue( "CRVAL3", fileInfo[0].crval3);
    outHeader.write( ofp);

    // do the actual concatenation
    qint64 buffSize = 1024 * 1024 * 512;
    char * buff = (char *) malloc( buffSize); assert( buff);
    QTime timer; timer.start(); QTime timer2; timer2.start();
    qint64 processed = 0;
    qint64 totalBytes = 0;
    for( size_t i = 0 ; i < fileInfo.size() ; i ++) {
        totalBytes += fileInfo[i].dataSize;
    }
    cerr << "Starting concatenation of " << formatBytes(totalBytes).toStdString() << "\n";
    for( size_t i = 0 ; i < fileInfo.size() ; i ++ ) {
        QString fname = fileInfo[i].fileName;
        cerr << "  appending " << fname.toStdString() << "\n";
        QFile fp( fname);
        if( ! fp.open( QFile::ReadOnly))
            throw QString( "Could not open file for reading: %1").arg( fname);
        // position the file to the offset
        if( ! fp.seek( fileInfo[i].dataOffset))
            throw QString( "Failed to seek to data segment: %1").arg( fname);
        qint64 remaining = fileInfo[i].dataSize;
        while( remaining > 0) {
            qint64 wantToRead = buffSize;
            if( remaining < wantToRead) wantToRead = remaining;
            // read in a chunk of input
            qint64 nRead = fp.read( buff, wantToRead);
            if( nRead <= 0)
                throw QString( "Failed to read from: %1").arg( fname);
            clipData( buff, nRead, -1000, 1000, fileInfo[i]);
            // write the chunk out
            if( ! blockWrite( ofp, buff, nRead))
                throw QString( "Failed to write to: %1").arg( outputFileName);
            remaining -= nRead;
            // statistics
            processed += nRead;
            if( timer2.elapsed() > 1000) {
                cerr << "    speed: " << (processed / 1024 / 1024) / (timer.elapsed() / 1000.0)
                     << " MB/s ";
                cerr << "wrote: " << formatBytes(ofp.pos()).toStdString() << "("
                     << (qint64)((processed * 100.0) / totalBytes) << "%) ";
                cerr << "elapsed: " << formatSeconds( timer.elapsed() / 1000.0).toStdString() << " ";
                double eta = (totalBytes - processed) * timer.elapsed() / processed / 1000;
                cerr << "eta: " << formatSeconds( eta).toStdString() << "\n";
                timer2.restart();
            }
        }
    }

    int pad = 2880 - ofp.pos() % 2880;
    if( pad > 0) {
        cerr << "Padding with " << pad << " bytes.\n";
        std::vector<char> buff(pad,0);
        if( ! blockWrite( ofp, buff.data(), pad)) {
            throw QString("Could not pad the output file.");
        }
    }
    else {
        cerr << "No padding needed.\n";
    }
    ofp.close();
    cerr << "Done.\n";
}
