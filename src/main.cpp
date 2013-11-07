#include <cstdlib>
#include <iostream>
#include <QtCore/QCoreApplication>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <QString>
#include <QStringList>
#include <QFile>
#include <QTextStream>
#include <QTime>

#include "extractor.h"

using namespace std;

static void usage( const QString & prog )
{
    cerr << QString( "Error! Usage: %1 output [list of fits files]\n").arg(prog).toStdString();
    exit( -1 );
}

int main( int argc, char ** argv)
{
    QCoreApplication app(argc, argv);

    // initialize random number generator
    qsrand(QTime(0,0,0).msecsTo(QTime::currentTime()));

    // get the command line arguments
    if( argc < 3 ) {
        usage( argv[0]);
    }
    QStringList inputFiles;
    for( int i = 2 ; i < argc ; i ++ )
        inputFiles << argv[i];
    QString outputFile = argv[1];
//    cerr << "Input files:\n";
//    for( int i = 0 ; i < inputFiles.size() ; i ++ )
//        cerr << QString("  %1 %2\n").arg(i,3).arg(inputFiles[i]).toStdString();
//    cerr << QString("Output file:\n  %1\n").arg(outputFile).toStdString();

    if( QFileInfo(outputFile).exists()) {
        cerr << "*** ERROR *** output file already exists, I refuse to overwrite it.\n";
        exit(-1);
    }


    bool success = false;
    try {
        combineFITS( inputFiles, outputFile );
        success = true;
    } catch ( const char * msg) {
        cerr << "Error: " << msg << "\n";
    } catch ( const QString & msg) {
        cerr << "Error: " << msg.toStdString() << "\n";
    } catch ( ... ) {
        cerr << "Error: unknown.\n";
    }
    if( ! success) {
        cerr << "Aborting...\n";
        exit(-1);
    }

    return 0;
}
