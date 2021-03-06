// Don't forget the number of chars in a kernal header from the start of the filename is 187, so 0 to 186 *inclusive*. This means there are 171 ($ab) bytes after the name.

/*
TapePilotCode1		= %11111111 ; Pilot tones, designed to fill the input accumulation register with a pattenr nothing like the sync byte
TapePilotCode2		= %11000011
TapeSyncCode		= %10101010 ; Used to sync just before reading data
TapeHeaderByteEx	= %11110000 ; This and TapeHeaderByteEx2 are used for normal length headers
TapeHeaderByteEx2	= %10010110
TapeHeaderByteEx3	= %01101001	; Used for very short headers after the sync
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <set>
#include <vector>
#include <string>
#include <algorithm>
#include "C64Tape.h"

/*******************
Useful links
http://wav-prg.sourceforge.net/faq.html
http://www.infinite-loop.at/Power20/Documentation/Power20-ReadMe/AE-File_Formats.html
http://c64tapes.org/dokuwiki/doku.php?id=analyzing_loaders#tap_format
http://c64tapes.org/dokuwiki/doku.php?id=loaders:rom_loader
*******************/

static const char *tapeMagic = "C64-TAPE-RAW";
static int sStreamPriority = 1;
static int sStreamFile = 1;

C64Tape::C64Tape()
{
	mObservedShortPulse = 0x30;
	mCalculatedMediumPulse = 0x42;
	mCalculatedLongPulse = 0x56;
	mTapeFileVersion = 1;
	mForWriting = false;
	mOpenedFileLength = 0;
	mTurboLittleEndian = false;

	mChecksumRegister = 0;

	SetDefaultLabels();
	ReadTurboLabels();

	mTurboGotSync = false;
	mTapeFile = 0;

	mCurrentStream = new Stream();
}

void C64Tape::SetDefaultLabels(void)
{
	mLabelToAddress["TapePilotCode1"]		= 0xff;
	mLabelToAddress["TapePilotCode2"]		= 0xc3;
	mLabelToAddress["TapeSyncCode"]			= 0xaa;
	mLabelToAddress["TapeHeaderByteEx"]		= 0xf0;
	mLabelToAddress["TapeHeaderByteEx2"]	= 0x96;
	mLabelToAddress["TapeHeaderByteEx3"]	= 0x69;

	mLabelToAddress["TapeTurboSpeed"]		= 0x80;
}

void C64Tape::ReadTurboLabels(void)
{
	mTapePilotCode1		= mLabelToAddress["TapePilotCode1"];
	mTapePilotCode2		= mLabelToAddress["TapePilotCode2"];
	mTapeSyncCode		= mLabelToAddress["TapeSyncCode"];
	mTapeHeaderByteEx	= mLabelToAddress["TapeHeaderByteEx"];
	mTapeHeaderByteEx2	= mLabelToAddress["TapeHeaderByteEx2"];
	mTapeHeaderByteEx3	= mLabelToAddress["TapeHeaderByteEx3"];

	mTapeTurboSpeed		= mLabelToAddress["TapeTurboSpeed"];
	mTurboZeroPulse		= (mTapeTurboSpeed / 8) * 2;
	mTurboOnePulse		= (mTapeTurboSpeed / 8) * 4;

	mTurboHeaderBytes.clear();
	mTurboHeaderBytes.push_back( mLabelToAddress["TapeHeaderByteEx"] ); // Full files or blocks have these two header bytes
	mTurboHeaderBytes.push_back( mLabelToAddress["TapeHeaderByteEx2"] );
//	mTurboHeaderBytes.push_back( 0x69 );	// This is a tiny header. No filename/start/end/header checksum etc
}

C64Tape::~C64Tape()
{
}

PulseTypes C64Tape::CommodoreReadPulse( unsigned char input , const bool updateShortPulse )
{
	if ( !input && mTapeFileVersion )
	{
		// Skip the next three
		unsigned char in;
		fread( &in , 1 , 1 , mTapeFile );
		fread( &in , 1 , 1 , mTapeFile );
		fread( &in , 1 , 1 , mTapeFile );
		input = in | in | in;	// Not actually correct, it should be a 24 bit value, but it skips the required number of bytes in the file and the actual length is not that important
		if ( !input )
		{
			return kShort;
		}
	}

	int timeShort = abs( mObservedShortPulse - int( input ) );
	int timeMedium = abs ( mCalculatedMediumPulse - int( input ) );
	int timeLong = abs ( mCalculatedLongPulse - int( input ) );

	PulseTypes ret = kShort;
	if ( ( timeShort < timeMedium ) && ( timeShort < timeLong ) )
	{
		ret = kShort;
#if 0	// Disable the short pulse calibration code for now, it doesn't work on all tapes.
		// If we are not reading a byte and if the short pulse time is within a sensible range then 
		if ( updateShortPulse && ( input > 8 )  && ( input < 0x80 ) )
		{
			if ( mObservedShortPulse != int( input ) )
			{
//				printf( "Updating short pulse to $%x                       \r" , int( input ) );
				mObservedShortPulse = int( input );
				// Scale up the rest of the values relative to the observed short pulse
				mCalculatedMediumPulse = ( mObservedShortPulse * 0x42 ) / 0x30;
				mCalculatedLongPulse = ( mObservedShortPulse * 0x56 ) / 0x30;
			}
		}
#endif
	}
	else if ( ( timeMedium < timeShort ) && ( timeMedium < timeLong ) )
	{
		ret = kMedium;
	}
	else
	{
		ret = kLong;
	}

	return ret;
}


PulseTypes C64Tape::TurboReadPulse( unsigned char input )
{
	if ( !input && mTapeFileVersion )
	{
		// Skip the next three
		unsigned char in;
		fread( &in , 1 , 1 , mTapeFile );
		fread( &in , 1 , 1 , mTapeFile );
		fread( &in , 1 , 1 , mTapeFile );
		input = in | in | in;	// Not actually correct, it should be a 24 bit value, but it skips the required number of bytes in the file and the actual length is not that important
		if ( !input )
		{
			return kOne;
		}
	}

	int timeZero = abs( mTurboZeroPulse - int( input ) );
	int timeOne = abs ( mTurboOnePulse - int( input ) );

	if ( ( timeZero < timeOne ) )
	{
		return kZero;
	}
	return kOne;
}

void C64Tape::TapeWriteByte( const bool useTurbo , const unsigned char output )
{
	if (useTurbo)
	{
		int bits;
		for ( bits = 7 ; bits >= 0 ; bits-- )
		{
			if ( output & (1 << bits) )
			{
//				fputc( mTurboOnePulse , mTapeFile );
				mCurrentStream->mData.push_back( (char) mTurboOnePulse );
			}
			else
			{
//				fputc( mTurboZeroPulse , mTapeFile );
				mCurrentStream->mData.push_back( (char) mTurboZeroPulse );
			}
		}
	}
	else
	{
//		fputc( mCalculatedLongPulse , mTapeFile );
//		fputc( mCalculatedMediumPulse , mTapeFile );
		mCurrentStream->mData.push_back( (char) mCalculatedLongPulse );
		mCurrentStream->mData.push_back( (char) mCalculatedMediumPulse );

		bool parity = true;

		int bits;
		for ( bits = 0 ; bits <= 7 ; bits++ )
		{
			if ( output & (1 << bits) )
			{
				parity = !parity;

//				fputc( mCalculatedMediumPulse , mTapeFile );
//				fputc( mObservedShortPulse , mTapeFile );
				mCurrentStream->mData.push_back( (char) mCalculatedMediumPulse );
				mCurrentStream->mData.push_back( (char) mObservedShortPulse );
			}
			else
			{
//				fputc( mObservedShortPulse , mTapeFile );
//				fputc( mCalculatedMediumPulse , mTapeFile );
				mCurrentStream->mData.push_back( (char) mObservedShortPulse );
				mCurrentStream->mData.push_back( (char) mCalculatedMediumPulse );
			}
		}

		if ( parity )
		{
//			fputc( mCalculatedMediumPulse , mTapeFile );
//			fputc( mObservedShortPulse , mTapeFile );
			mCurrentStream->mData.push_back( (char) mCalculatedMediumPulse );
			mCurrentStream->mData.push_back( (char) mObservedShortPulse );
		}
		else
		{
//			fputc( mObservedShortPulse , mTapeFile );
//			fputc( mCalculatedMediumPulse , mTapeFile );
			mCurrentStream->mData.push_back( (char) mObservedShortPulse );
			mCurrentStream->mData.push_back( (char) mCalculatedMediumPulse );
		}

	}

	mChecksumRegister = mChecksumRegister ^ output;
}

bool C64Tape::IsNum(const char *arg)
{
	if (!arg)
	{
		return false;
	}

	if ( ( arg[0] == 'l' ) || ( arg[0] == 'h' ) )
	{
		arg++;
	}

	if ( arg[0] == '.' )
	{
		if ( mLabelToAddress.find( std::string( arg + 1 ) ) != mLabelToAddress.end() )
		{
			return true;
		}
		printf( "ERROR: Argument 'arg' is not a valid label.\n" , arg );
		exit(-1);
		return false;
	}
	if ( ( arg[0] == '!' ) || ( arg[0] == '$' ) || isdigit( arg[0] ) )
	{
		return true;
	}
	return false;
}

int C64Tape::ParamToNum(const char *arg)
{
	if (!arg)
	{
		return 0;
	}

	bool isLow = false;
	bool isHigh = false;

	if ( arg[0] == 'l' )
	{
		arg++;
		isLow = true;
	}
	if ( arg[0] == 'h' )
	{
		arg++;
		isHigh = true;
	}

	int num = 0;
	if(arg[0]=='.')
	{
		std::map<std::string , int>::iterator found = mLabelToAddress.find( std::string( arg + 1 ) );
		if ( found != mLabelToAddress.end() )
		{
			num = found->second;
		}
		else
		{
			printf( "ERROR: Argument '%s' is not a valid label.\n" , arg );
			exit(-1);
			return -1;
		}
	}
	else if(arg[0]=='!')
	{
		num = 0;
		const char *pos = arg+1;
		while ( ( *pos == '0' ) || ( *pos == '1' ) )
		{
			num <<= 1;
			if ( *pos == '1' )
			{
				num |= 1;
			}
			pos++;
		}
	}
	else if(arg[0]=='$')
	{
		num = strtol(arg+1,NULL,16);
	}
	else if((arg[0]=='0')&&((arg[1]&0xdf)=='X'))
	{
		num = strtol(arg+2,NULL,16);
	}
	else
	{
		num = strtol(arg,NULL,10);
	}

	if ( isLow )
	{
		return num & 0xff;
	}
	else if ( isHigh )
	{
		return ( num >> 8 ) & 0xff;
	}
	return num;
}

int C64Tape::HandleParams( int argc , char ** argv )
{
	bool opened = false;
	const char *exeArgv = argv[0];

	if ( argc <= 1 )
	{
		DisplayHelp();
		return -1;
	}

	printf( "Input %d parameters: " , argc - 1 );
	int i;
	for ( i= 1 ; i < argc ; i++ )
	{
		if ( strchr( argv[ i ] , ' ' ) )
		{
			printf( "\"%s\" " , argv[ i ] );
		}
		else
		{
			printf( "%s " , argv[ i ] );
		}
	}
	printf( "\n" );

	while (argc > 0)
	{
		argc--;	// First time around this skips the EXE path
		argv++;

		if ( argc <= 0 )
		{
			break;
		}

		printf( "Processing param '%s'\n" , *argv );

		switch(argv[0][0])
		{
			case 'h':
			case '?':
			default:
			{
				DisplayHelp();
				break;
			}

			case 'r':
			{
				if (mTapeFile)
				{
					fclose(mTapeFile);
				}
				mTapeFile = 0;

				argc--;
				argv++;

				if ( argc <= 0 )
				{
					printf("Error: Filename expected\n");
					exit(-1);
				}

				mTapeFile = fopen( *argv , "rb" );
				if ( !mTapeFile )
				{
					printf( "Couldn't open '%s' for reading.\n" , *argv);
					exit(-1);
				}

				opened = true;
				mForWriting = false;
				mTurboGotSync = false;
				mChecksumRegister = 0;

				unsigned char buffer[20];
				int ret = fread( buffer , 1 , 20 , mTapeFile );

				if ( strncmp( tapeMagic , (char *) buffer , 12 ) )
				{
					printf( "'%s' Doesn't look like a TAP file.\n" , *argv );
					exit(-2);
				}

				mTapeFileVersion = (int) buffer[12];

				mOpenedFileLength = int(buffer[16]) | (int(buffer[17])<<8) | (int(buffer[18])<<16) | (int(buffer[19])<<24);

				printf( "\nOpened '%s' for reading.\n" , *argv );
				break;
			}

			case 'w':
			{
				if (mTapeFile)
				{
					fclose(mTapeFile);
				}
				mTapeFile = 0;

				const char *mode = "rb+";
				if ( argv[0][1] == 'n' )
				{
					mode = "wb+";
				}

				argc--;
				argv++;

				if ( argc <= 0 )
				{
					printf("Error: Filename expected\n");
					exit(-1);
				}

				mTapeFile = fopen( *argv , mode );
				if (!mTapeFile)
				{
					mTapeFile = fopen( *argv , "wb+" );
				}
				if ( !mTapeFile )
				{
					printf( "Couldn't open '%s' for writing.\n" , *argv );
					exit(-1);
				}

				opened = true;
				mForWriting = true;
				mTurboGotSync = false;
				mChecksumRegister = 0;

				fseek( mTapeFile , 0 , SEEK_END );

				int pos = ftell( mTapeFile );
				if (pos == 0)
				{
					fwrite( tapeMagic , 1 , 12 , mTapeFile );

					fwrite( &mTapeFileVersion , 1 , 1 ,mTapeFile );
					unsigned char blank = 0;
					fwrite( &blank , 1 , 1 ,mTapeFile );
					fwrite( &blank , 1 , 1 ,mTapeFile );
					fwrite( &blank , 1 , 1 ,mTapeFile );

					int len = 0;
					fwrite( &len , 4 , 1 ,mTapeFile );
				}

				fseek( mTapeFile , 20 , SEEK_SET );

				printf( "\nOpened '%s' for writing.\n" , *argv );
				break;
			}

			case 'c':
			{
				if ( !mTapeFile || !mForWriting )
				{
					printf( "No file opened.\n" );
					exit(-1);
				}

				FlushStreams();
				fflush( mTapeFile );
				fseek( mTapeFile , 0 ,SEEK_END );
				int len = ftell( mTapeFile );
				len -= 20;
				fseek( mTapeFile , 16 ,SEEK_SET );
				fflush( mTapeFile );
				fputc( len & 255 , mTapeFile );
				len >>= 8;
				fputc( len & 255 , mTapeFile );
				len >>= 8;
				fputc( len & 255 , mTapeFile );
				len >>= 8;
				fputc( len & 255 , mTapeFile );
				fflush( mTapeFile );

				fclose(mTapeFile);

				mTapeFile = 0;
				break;
			}

			case 'm':
			{
				if ( argv[0][1] == 'c' )
				{
					mLabelToAddress.clear();
					SetDefaultLabels();
				}

				argc--;
				argv++;

				if ( argc <= 0 )
				{
					printf("Error: Filename expected\n");
					exit(-1);
				}

				FILE *fp = fopen( *argv , "r" );
				if ( !fp )
				{
					printf( "Couldn't open map file '%s' for reading.\n" , *argv );
					exit(-1);
				}

				while ( !feof( fp ) )
				{
					char buffer[1024];
					buffer[0] = '\0';
					if ( fgets( buffer , sizeof(buffer) - 1 , fp ) )
					{
						std::string label;
						int addr;
						const char *delim = " =;\x0d\x0a\x09";
						label = strtok( buffer , delim );
						addr = ParamToNum( strtok( 0 , delim ) );
						mLabelToAddress[ label ] = addr;
					}
				}

				fclose(fp);

				ReadTurboLabels();
				break;
			}

			case 'p':
			{
				if ( !mTapeFile )
				{
					printf( "No file opened.\n" );
					exit(-1);
				}

				argc--;
				argv++;

				if ( argc <= 0 )
				{
					printf("Error: Position expected\n");
					exit(-1);
				}

				int thePos = ParamToNum( *argv );
				fseek( mTapeFile , thePos , SEEK_SET );

				mTurboGotSync = false;

				break;
			}

			case 'a':
			{
				if ( !mTapeFile )
				{
					printf( "No file opened.\n" );
					exit(-1);
				}
				fflush( mTapeFile );
				fseek( mTapeFile , 0 , SEEK_END );
				fflush( mTapeFile );
				break;
			}

			case 'f':
			{
				if ( argv[0][1] == 't' )
				{
					argc--;
					argv++;

					if ( argc <= 0 )
					{
						printf("Error: Turbo zero pulse length expected\n");
						exit(-1);
					}

					mTurboZeroPulse = ParamToNum(*argv) / 8;
					mTapeTurboSpeed = ( mTurboZeroPulse * 8 ) / 2;	// Converts the tape turbo speed from the zero pulse length

					argc--;
					argv++;

					if ( argc <= 0 )
					{
						printf("Error: Turbo one pulse length expected\n");
						exit(-1);
					}

					mTurboOnePulse = ParamToNum(*argv) / 8;

					argc--;
					argv++;

					if ( argc <= 0 )
					{
						printf("Error: Turbo little endian flag expected\n");
						exit(-1);
					}

					int endian = ParamToNum(*argv);
					mTurboLittleEndian = (endian == 0);

					argc--;
					argv++;

					if ( argc <= 0 )
					{
						printf("Error: Turbo zero sync code expected\n");
						exit(-1);
					}

					mTapeSyncCode = (unsigned char) ParamToNum(*argv);

					mTurboGotSync = false;
					break;
				}
				break;
			}

			case 'x':
			{
				mChecksumRegister = 0;

				if ( ( argc > 1 ) && IsNum( argv[1] ) )
				{
					argc--;
					argv++;
					mChecksumRegister = ParamToNum(*argv);
				}
				break;
			}

			case 'b':
			{
				if ( !mTapeFile )
				{
					printf( "No file opened.\n" );
					exit(-1);
				}

				int rep = 1;
				if ( argv[0][1] )
				{
					if ( IsNum( argv[0] + 2 ) )
					{
						rep = ParamToNum(argv[0]+2);
						if ( rep <= 0 )
						{
							rep = 1;
						}
					}
				}

				bool gotOneGoodByte = false;
				if ( argv[0][1] == 't' )
				{
					mTurboGotSync = false;

					// Turbo format
					unsigned char rollingByteInput = 0;
					unsigned char rollingChecksum = mTapeSyncCode;
					int gotBits = 0;
					std::vector<unsigned char> bytesGot;
					bytesGot.reserve(128);
					while ( rep > 0 )
					{
						unsigned char in1;

						int tpos = ftell( mTapeFile );
						if ( tpos >= ( mOpenedFileLength + 20 ) )
						{
							printf( "End of file reached by data.                                    \n" );
							break;
						}

						int numRead = fread( &in1 , 1 , 1 , mTapeFile );

						if ( numRead )
						{
							// Emulate what the C64 tape code does and roll around the input byte
							PulseTypes p1 = TurboReadPulse( in1 );

							if ( mTurboLittleEndian )
							{
								rollingByteInput = rollingByteInput >> 1;
								if ( p1 == kOne )
								{
									rollingByteInput |= 1 << 7;
								}
							}
							else
							{
								rollingByteInput = rollingByteInput << 1;
								if ( p1 == kOne )
								{
									rollingByteInput |= 1;
								}
							}

							// First check for sync byte if it hasn't arrived yet
							if ( !mTurboGotSync )
							{
								if ( rollingByteInput == mTapeSyncCode )
								{
									printf( "\nGot turbo sync byte at $%x\n" , ftell( mTapeFile ) );
									mTurboGotSync = true;
									gotBits = 0;
									gotOneGoodByte = false;
									rollingByteInput = 0;
									rollingChecksum = mTapeSyncCode;
									bytesGot.clear();
								}
								continue;
							}

							gotBits++;
							if ( gotBits < 8 )
							{
								continue;
							}

							rollingChecksum = rollingChecksum ^ rollingByteInput;

							gotBits = 0;
							gotOneGoodByte = true;
							printf( " $%02x" , rollingByteInput );
							bytesGot.push_back( rollingByteInput );

							mChecksumRegister = mChecksumRegister ^ rollingByteInput;

							if ( bytesGot.size() == mTurboHeaderBytes.size() )
							{
								if ( bytesGot != mTurboHeaderBytes )
								{
//										printf( "\nFailed extended header check $%x. No sync.\n" , ftell( mTapeFile ) );
									mTurboGotSync = false;
									rollingByteInput = 0;
									rep += bytesGot.size();
									bytesGot.clear();
									continue;
								}
							}
							else if ( bytesGot.size() == ( mTurboHeaderBytes.size() + 7 ) )
							{
								// Do the extended header checksum check. A checksum of the header data received so far.
								// Since the checksum byte has just been read and XORed with the calculated checksum it should be 0 at this point if the calculated and read checksum match
								// Header bytes, filename byte, start lo/hi, expected num blocks (optional), end lo/hi, header checksum (optional)
								// Don't forget, in block checksum mode each 256 bytes of the file are saved with the full filename structure. For example, a file 1000 bytes long is split into 4 separate "files".
								if ( rollingChecksum == 0 )
								{
									printf( "\nExtended header checksum check $%x OK!\n" , ftell( mTapeFile ) );
								}
								else
								{
//										printf( "\nFailed extended header checksum check $%x. No sync.\n" , ftell( mTapeFile ) );
									mTurboGotSync = false;
									rollingByteInput = 0;
									rep += bytesGot.size();
									bytesGot.clear();
									continue;
								}
							}

							rollingByteInput = 0;
						}
						else
						{
							printf( "End of file reached.                                    \n" );
							break;
						}

						rep--;
					} //< while ( rep > 0 )
				}
				else if ( argv[0][1] == 'c' )
				{
					mTurboGotSync = false;

					// Commodore format
					while ( rep > 0 )
					{
						unsigned char in1 , in2;

						int tpos = ftell( mTapeFile );
						if ( tpos >= ( mOpenedFileLength + 20 ) )
						{
							printf( "End of file reached by data.                                    \n" );
							break;
						}

						int numRead = fread( &in1 , 1 , 1 , mTapeFile );

						if ( numRead )
						{
							// First check for data start
							PulseTypes p1 = CommodoreReadPulse( in1 , true );
							if ( p1 != kLong )
							{
								// Not long so skip
								if (gotOneGoodByte)
								{
									printf( "\nSkipping offset $%x short pulse at $%x\n" , ftell( mTapeFile ) , mObservedShortPulse );
								}
								gotOneGoodByte = false;
								continue;
							}
							numRead = fread( &in2 , 1 , 1 , mTapeFile );
							if ( !numRead )
							{
								printf( "End of file reached mid kernal pulse.                       \n" );
								break;
							}

							PulseTypes p2 = CommodoreReadPulse( in2 );

							if ( ( p1 == kLong ) && ( p2 == kMedium ) )
							{
								// Start bit pair encountered
								int theByte = 0;
								int times = 8;
								bool calcParity = true;

								// Read 8 bit pairs
								while (times)
								{
									numRead = fread( &in1 , 1 , 1 , mTapeFile );
									if (numRead)
									{
										numRead = fread( &in2 , 1 , 1 , mTapeFile );
									}
									if ( !numRead )
									{
										printf( "End of file reached mid byte.                       \n" );
										break;
									}
									p1 = CommodoreReadPulse( in1 );
									p2 = CommodoreReadPulse( in2 );

									if ( ( p1 == kShort ) && ( p2 == kMedium ) )
									{
										theByte = theByte >> 1;
									}
									else if ( ( p1 == kMedium ) && ( p2 == kShort ) )
									{
										theByte = (theByte >> 1) | 0x80;
										calcParity = !calcParity;
									}
									else
									{
										printf( "Unknown pulse pair type encountered %d:%d       \n" , p1 , p2 );
										// Rewind one pulse and try again
										fseek( mTapeFile , -1 , SEEK_CUR );	// Rewind to try to regain tape byte sync
										break;
									}
									times--;
								} //< while (times--)
								if ( times )
								{
									// Some error happened so continue to the next byte
									continue;
								}
								// Read parity
								numRead = fread( &in1 , 1 , 1 , mTapeFile );
								if (numRead)
								{
									numRead = fread( &in2 , 1 , 1 , mTapeFile );
								}
								if ( !numRead )
								{
									printf( "End of file reached mid parity byte.                   \n" );
									break;
								}

								p1 = CommodoreReadPulse( in1 );
								p2 = CommodoreReadPulse( in2 );

								bool readParity = false;
								if ( ( p1 == kShort ) && ( p2 == kMedium ) )
								{
									readParity = false;
								}
								else if ( ( p1 == kMedium ) && ( p2 == kShort ) )
								{
									readParity = true;
								}
								else
								{
									printf( "Unknown parity bit pair type encountered %d:%d\n" , p1 , p2 );
									// Rewind one pulse and try again
									fseek( mTapeFile , -1 , SEEK_CUR );	// Rewind to try to regain tape byte sync
									continue;
								}

								if ( calcParity != readParity )
								{
									printf( "Parity mismatch                              \n" );
									// Rewind one pulse and try again
									fseek( mTapeFile , -1 , SEEK_CUR );	// Rewind to try to regain tape byte sync
									continue;
								}

								if (!gotOneGoodByte)
								{
									printf( "\nNew data at offset $%x short pulse at $%x\n" , ftell( mTapeFile ) , mObservedShortPulse );
								}
								gotOneGoodByte = true;
								printf( " $%02x" , theByte );

								mChecksumRegister = mChecksumRegister ^ theByte;
							}
							else if ( ( p1 == kLong ) && ( p2 == kShort ) )
							{
								if ( gotOneGoodByte )
								{
									printf( " EOD\n" );
									gotOneGoodByte = false;
								}
							}
							else
							{
								// Rewind one pulse and try again
								fseek( mTapeFile , -1 , SEEK_CUR );	// Rewind to try to regain tape byte sync
								continue;
							}
						}
						else
						{
							printf( "End of file reached.                                    \n" );
							break;
						}

						rep--;
					} //< while ( rep > 0 )
				}
				break;
			}


			case 'o':
			{
				if ( !mForWriting || !mTapeFile )
				{
					printf( "No file opened for writing.\n" );
					exit(-1);
				}

				AddStream(false);
				sStreamFile++;

				bool useTurbo = false;
				if ( argv[0][1] == 'c' )
				{
					// Kernal
					useTurbo = false;
				}
				else if ( argv[0][1] == 't' )
				{
					// Turbo
					useTurbo = true;
				}

				int rep = 1;
				// Potential three parameter combinations first
				if ( argv[0][1] && argv[0][2] && IsNum( argv[0] + 3 ) )
				{
					rep = ParamToNum(argv[0]+3);
				}
				else if ( argv[0][1] && IsNum( argv[0] + 2 ) )
				{
					// Now potential two letters
					rep = ParamToNum(argv[0]+2);
				}
				if ( rep <= 0 )
				{
					rep = 1;
				}
				if ( argv[0][1] && argv[0][2] == 'b' && argv[0][3] == '1' )
				{
					// Construct a new command to execute, recursively
					std::string argvs[] = {
						// Tape header
						"ocl$6a10" , "ocn" , "$89" , "$88" , "$87" , "$86" , "$85" , "$84" , "$83" , "$82" , "$81" , "ocnr" , "$03" , "l.TapeRelocatedStart" , "h.TapeRelocatedStart" , "$16" , "$03" , "@\"Loaders.bin\"" , ".nameStart" , ".nameEnd" , "@\"Loaders.bin\"" , ".tapeHeader" , ".tapeHeaderEnd" , "!" ,"oce" ,
						// File data, no duplicate @ 25
						"ocl$1500" , "ocn" , "$89" , "$88" , "$87" , "$86" , "$85" , "$84" , "$83" , "$82" , "$81" , "ocnr" , "@\"Loaders.bin\"" , ".startBlock" , ".TapeTurboEndOfExtendedZeroPageCodeAndIRQ" , "!" , "oce"
					};
					const int largc = sizeof(argvs) / sizeof(argvs[0]);

					argc --;
					argv ++;

					// Update the parameters with anything passed in
					argvs[17] = std::string( "@" ) + std::string( argv[0] );
					argvs[20] = std::string( "@" ) + std::string( argv[0] );
					argvs[37] = std::string( "@" ) + std::string( argv[0] );

					if ( (argc >= 7) && IsNum( argv[1] ) && IsNum( argv[2] ) && IsNum( argv[3] ) && IsNum( argv[4] ) && IsNum( argv[5] ) && IsNum( argv[6] ) && IsNum( argv[7] ) )
					{
						argvs[13] = std::string( "l" ) + std::string( argv[1] );
						argvs[14] = std::string( "h" ) + std::string( argv[1] );
						argvs[18] = std::string( argv[2] );
						argvs[19] = std::string( argv[3] );
						argvs[21] = std::string( argv[4] );
						argvs[22] = std::string( argv[5] );

						argvs[38] = std::string( argv[6] );
						argvs[39] = std::string( argv[7] );

						argc -= 7;
						argv += 7;
					}

					const char *largvs[256];
					int i;
					memset( largvs , 0 , sizeof( largvs ) );

					largvs[0] = exeArgv;
					for ( i = 1 ; i <= 25 ; i++ )
					{
						largvs[ i ] = argvs[ i - 1 ].c_str();
					}
					HandleParams( 26 , (char **) largvs );

					argvs[0] = "ocl$4f";
					argvs[2] = "$09";
					argvs[3] = "$08";
					argvs[4] = "$07";
					argvs[5] = "$06";
					argvs[6] = "$05";
					argvs[7] = "$04";
					argvs[8] = "$03";
					argvs[9] = "$02";
					argvs[10] = "$01";

					for ( i = 1 ; i <= 42 ; i++ )
					{
						largvs[ i ] = argvs[ i - 1 ].c_str();
					}
					HandleParams( 43 , (char **) largvs );

					AddStream();
					break;
				}
				else if ( argv[0][1] && argv[0][2] == 'l' )
				{
					// Write a leader
					while ( rep-- )
					{
						if ( useTurbo )
						{
							TapeWriteByte( true , mTapePilotCode1 );
							TapeWriteByte( true , mTapePilotCode2 );
						}
						else
						{
//							fputc( (char) mObservedShortPulse , mTapeFile );
							mCurrentStream->mData.push_back( (char) mObservedShortPulse );
						}
					}

					AddStream();
				}
				else if ( argv[0][1] && argv[0][2] == 'e' )
				{
					// Write an end-of-data marker
					while ( rep-- )
					{
						if ( useTurbo )
						{
							unsigned char tempChecksum = 0;
							TapeWriteByte( true , mTapePilotCode1 );
							TapeWriteByte( true , mTapePilotCode2 );
						}
						else
						{
//							fputc( (char) mCalculatedLongPulse , mTapeFile );
//							fputc( (char) mObservedShortPulse , mTapeFile );
							mCurrentStream->mData.push_back( (char) mCalculatedLongPulse );
							mCurrentStream->mData.push_back( (char) mObservedShortPulse );
						}
					}

					AddStream();
				}
				else if ( argv[0][1] && argv[0][2] == 'n' )
				{
					if ( argv[0][3] == 'r' )
					{
						mChecksumRegister = 0;
					}

					argc--;
					argv++;

					// Write bytes
					while ( (argc > 0 ) && ( IsNum( *argv ) || (argv[0][0] == '@') || (argv[0][0] == '!') ) )
					{
						// Handle a checksum byte request
						if( argv[0][0] == '!' )
						{
							TapeWriteByte( useTurbo , mChecksumRegister );
						}
						else if( ( argv[0][0] == '@' ) && ( argc >= 2 ) )
						{
							// Handle a file request
							std::string filename(argv[0]+1);

							argc--;
							argv++;
							int start = ParamToNum( *argv );
							argc--;
							argv++;
							int end = ParamToNum( *argv );

							size_t pos;
							while ( (pos = filename.find_first_of( '\"' )) != std::string::npos )
							{
								filename.erase( pos , 1 );
							}

							FILE *inFp = fopen( filename.c_str() , "rb" );
							if ( inFp )
							{
								printf( "Writing bytes from file '%s' from $%x to $%x\n" , filename.c_str() , start , end );

								fseek( inFp , start , SEEK_SET );
								while( ( ftell(inFp) != end ) && !feof( inFp ) )
								{
									int got = fgetc( inFp );
									
									TapeWriteByte( useTurbo , got );
								}

								fclose( inFp );
							}
							else
							{
								printf( "Couldn't open '%s' for reading.\n" , filename.c_str() );
							}
						}
						else
						{
							int got = ParamToNum( *argv );
							TapeWriteByte( useTurbo , got );
						}

						argc--;
						argv++;
					} //< while ( IsNum( *argv ) || (argv[0][1] == '@') )
					argc++;
					argv--;

					AddStream();
				}
				else if ( argv[0][1] && argv[0][2] == 'f' )
				{
					bool checksumBlocks = false;
					bool tinyHeader = false;
					if ( argv[0][3] == 'b' )
					{
						checksumBlocks = true;
					}
					else if ( argv[0][3] == 't' )
					{
						tinyHeader = true;
					}

					mChecksumRegister = 0;

					argc--;
					argv++;
					char *filename = *argv;

					FILE *inFp = fopen( filename , "rb" );
					if ( !inFp )
					{
						printf( "Couldn't open file '%s' for reading.\n" , inFp );
						break;
					}

					int filenameByte = 0;
					if ( !tinyHeader )
					{
						argc--;
						argv++;
						filenameByte = ParamToNum(*argv);
					}

					int startOffset = 0;
					int endOffset = 0;
					int startAddress = 0;

					bool numsParsed = false;
					if ( tinyHeader )
					{
						if ( IsNum( argv[1] ) && IsNum( argv[2] ) )
						{
							startOffset = ParamToNum( argv[1] );
							endOffset = ParamToNum( argv[2] );

							argc -= 2;
							argv += 2;
							numsParsed = true;
						}
					}
					else
					{
						if ( IsNum( argv[1] ) && IsNum( argv[2] ) && IsNum( argv[3] ) )
						{
							startOffset = ParamToNum( argv[1] );
							endOffset = ParamToNum( argv[2] );
							startAddress = ParamToNum( argv[3] );

							argc -= 3;
							argv += 3;
							numsParsed = true;
						}
					}
					if ( !numsParsed )
					{
						fseek( inFp , 0 , SEEK_END );
						endOffset = ftell( inFp );
						fseek( inFp , 0 , SEEK_SET );
						startOffset = 2;

						if ( !tinyHeader )
						{
							startAddress = fgetc( inFp ) & 0xff;
							startAddress |= ( fgetc( inFp ) & 0xff ) << 8;
						}
					}
					int endAddress = startAddress + (endOffset - startOffset);

					printf( "Writing file '%s' from $%x to $%x load $%x chk %d filename byte %d\n" , filename , startOffset , endOffset , startAddress , checksumBlocks , filenameByte );

					fseek( inFp , startOffset , SEEK_SET );

					int numBlocks = ( ( ( endOffset - 1 )  - startOffset ) / 256 ) + 1;

					WriteTurboLeader();

					// Write bytes
					if ( checksumBlocks )
					{
						int thisBlock = startAddress;
						bool firstBlock = true;

						while ( thisBlock < endAddress )
						{
							mChecksumRegister = 0;
							int thisBlockSize = endAddress - thisBlock;
							if ( thisBlockSize > 256 )
							{
								thisBlockSize = 256;
							}
							int thisBlockEnd = thisBlock + thisBlockSize;

							TapeWriteByte( true , mTapeSyncCode );
							TapeWriteByte( true , mTapeHeaderByteEx );
							TapeWriteByte( true , mTapeHeaderByteEx2 );

							TapeWriteByte( true , filenameByte );

							TapeWriteByte( true , thisBlock & 0xff );
							TapeWriteByte( true , (thisBlock >> 8) & 0xff );

							TapeWriteByte( true , numBlocks );

							TapeWriteByte( true , thisBlockEnd & 0xff );
							TapeWriteByte( true , (thisBlockEnd >> 8) & 0xff );

							TapeWriteByte( true , mChecksumRegister );

							int tempSize = thisBlockSize;
							while( ( tempSize > 0 ) && !feof( inFp ) )
							{
								int got = fgetc( inFp );

								TapeWriteByte( true , got );

								tempSize--;
							}

							TapeWriteByte( true , mChecksumRegister );

							WriteTurboLeader();

							AddStream(firstBlock);
							firstBlock = false;

							thisBlock += thisBlockSize;
						} //< while ( thisBlock < endAddress )

					}
					else
					{
						WriteTurboLeader();

						if ( tinyHeader )
						{
							TapeWriteByte( true , mTapeSyncCode );
							TapeWriteByte( true , mTapeHeaderByteEx3 );
						}
						else
						{
							TapeWriteByte( true , mTapeSyncCode );
							TapeWriteByte( true , mTapeHeaderByteEx );
							TapeWriteByte( true , mTapeHeaderByteEx2 );

							TapeWriteByte( true , filenameByte );

							TapeWriteByte( true , startAddress & 0xff );
							TapeWriteByte( true , (startAddress >> 8) & 0xff );

							TapeWriteByte( true , numBlocks );

							TapeWriteByte( true , endAddress & 0xff );
							TapeWriteByte( true , (endAddress >> 8) & 0xff );

							TapeWriteByte( true , mChecksumRegister );
						}

						mChecksumRegister = 0;

						while( ( ftell(inFp) != endOffset ) && !feof( inFp ) )
						{
							int got = fgetc( inFp );

							TapeWriteByte( true , got );
						}

						TapeWriteByte( true , mChecksumRegister );

						WriteTurboLeader();

						AddStream();
					}
				}

				break;
			}

			case 's':
			{
				if ( ( argc > 1 ) && IsNum( argv[1] ) )
				{
					argc--;
					argv++;
					sStreamPriority -= ParamToNum(*argv);
				}
				else
				{
					sStreamPriority--;
				}

				printf("Stream priority now %d\n" , sStreamPriority );
				break;
			}

		} //< switch
	}

	if (opened && mTapeFile)
	{
		fclose(mTapeFile);
	}

	printf("\n");

	return 0;
}

void C64Tape::WriteTurboLeader(int rep)
{
	while ( rep-- )
	{
		unsigned char tempChecksum = 0;
		TapeWriteByte( true , mTapePilotCode1 );
		TapeWriteByte( true , mTapePilotCode2 );
	}
}

void C64Tape::DisplayHelp(void)
{
	printf("TapeTool help.\n\
Any numbers can be in decimal 123 or hex $67 0x23 or binary !101101 format or if prefixed with a '.' can be a label from a read map file. A value can also be prefixed with 'l' or 'h' indicating the low or high byte.\n\
r : Open a tape file for reading.\n\n\
w[n] : Open a tape file for writing. If 'n' is supplied the file is created as a new file.\n\n\
a : Start to append data to the tape file.\n\n\
ft <pulse width 0> <pulse width 1> <endian 0/1> <sync byte> : Default 0x100 0x200 1 0xaa : Sets the tape turbo parameters to be used. The pulse width 0 and 1 parameters define the timing in cycles for 0 and 1 bits. The endian is little if 0 and big if 1. There must be one sync byte which signifies the start pattern of bits to use for loading subsequent bytes.\n\n\
x <value>: Reset the checksum XOR register to the optional value or zero by default. This can be used by both the kernal and turbo file writers. Every byte read/written after this will accumulate into the checksum register.\n\n\
t : If reading or writing truncate the tape file to the current position.\n\n\
p <position> : Skip to the file position.\n\n\
c : Close the file. If writing to a file then this must be used to close the file properly.\n\n\
m[c] <file name> : Read a map file from ACME of \"label = address\" pairs. The 'c' will optionally clear the map entries before reading the file. \n\n\
\n\n\
For the commands below: The rep is an optional number defining how many times to read/write.\n\
The bytes can be a combination of space separated numbers or a file name followed by start offset and end offset or \"!\" to write the checksum register. For example \"ocn $89 28 @\"file name\" 4 0x20 !\" will write the two bytes, followed by the bytes from the file starting at offset 4 and ending at offset 32 followed by the checksum register.\n\
bc[rep] : Read a byte in kernal format. When reading any leader information is skipped.\n\n\
bt[rep] : Read a byte in tape turbo format. Any leader/sync information skipped.\n\n\
Kernal format writes:\n\
ocl[rep] : Write a kernal format leader pulse. Typical values for rep are: $6a10 10 seconds for the start of the tape. $4f between repeated header blocks. $1500 between the header and the data blocks.\n\n\
oce[rep] : Write a kernal format end-of-data marker. There usually needs to be only one.\n\n\
ocn[r] [bytes]: Write tape bytes in kernal format. If 'r' is supplied the checksum will be reset.\n\n\
ocb1 <file name> [TapeRelocatedStart address to load data] [nameStart offset] [nameEnd offset] [tapeHeader offset] [tapeHeaderEnd offset] [startBlock offset] [TapeTurboEndOfExtendedZeroPageCodeAndIRQ offset]: Writes a turbo tape boot loader kernal file from \"file name\" with tape header containing user data without the second data block to improve loading speed. If the values are missing then the loaded label values are used.\n\n\
Turbo format writes:\n\
otl[rep] : Write a turbo format leader. Typical values for rep are: $6a10 10 seconds for the start of the tape. $2 between blocks. $c0 before the file to account for the tape motor starting.\n\n\
ote[rep] : Write a kernal format end-of-data marker. There usually needs to be only one.\n\n\
otn[r] [bytes] : Write tape bytes in turbo format.  If 'r' is supplied the checksum will be reset.\n\n\
otf[b] <file name> <file name byte> [start offset] [end offset] [load address] : Writes turbo data. If 'b' is added it will write checksum blocks rather than a whole file checksum. The checksum register is always reset. The start offset, end offset and load are optional and if the file is a PRG format file the load address is taken from the first two bytes.\n\n\
otft <file name> [start offset] [end offset] : Writes turbo data with a tiny header without a filename or a load address. The checksum register is always reset. The start offset and end offset are optional, they are calculated assuming a PRG format file.\n\n\
s : Interleave the next file with the previous file when using turbo checksum block method.\n\n\
");
}

C64Tape::Stream::Stream() : mPriority(0) , mFile(0) , mNewfile(false)
{
	mData.reserve(4096);
}

C64Tape::Stream::~Stream()
{
}

void C64Tape::AddStream(const bool newFile)
{
	mCurrentStream->mNewfile = newFile;
	mCurrentStream->mFile = sStreamFile;

	if ( !mCurrentStream->mData.empty() )
	{
		if ( newFile )
		{
			sStreamPriority++;
		}
		mCurrentStream->mPriority = sStreamPriority;
		mStreams.push_back( mCurrentStream );
		mCurrentStream = new Stream();
	}
}

void C64Tape::FlushStreams(void)
{
	AddStream(false);
	sStreamPriority++;

	// Spot streams with the same ID and interleave them
	std::vector<int> duplicateIDs;
	std::vector<std::list< Stream* >::iterator> duplicateIDIters;
	std::multimap< int , std::list< Stream* >::iterator > IDIters;
	duplicateIDs.resize( sStreamPriority , 0);
	duplicateIDIters.resize( sStreamPriority , mStreams.end() );
	std::list< Stream* >::iterator st = mStreams.begin();
	for( st = mStreams.begin() ; st != mStreams.end() ; )
	{
		Stream *stream = *st;
		if ( stream->mNewfile )
		{
			if ( duplicateIDs[ stream->mPriority ] == 0)
			{
				duplicateIDIters[ stream->mPriority ] = st;
			}
			duplicateIDs[ stream->mPriority ]++;
			IDIters.insert( std::pair< int , std::list< Stream* >::iterator>( stream->mPriority , st ) );
		}
		st++;
	}

	// Now look for duplicate IDs for each new "file"
	size_t i;
	for ( i = 0 ; i < duplicateIDs.size() ; i++ )
	{
		if ( duplicateIDs[i] <= 1 )
		{
			continue;
		}
		printf("Interleaving stream %d for sub-streams " , i);

		std::list< Stream* > newOrder;
		// If there are duplicate IDs then process them
		std::pair< std::multimap< int , std::list< Stream* >::iterator >::iterator , std::multimap< int , std::list< Stream* >::iterator >::iterator > range;
		range = IDIters.equal_range( i );
		std::for_each( range.first , range.second , [](std::pair< int , std::list< Stream* >::iterator > entry )
		{
			printf( "%d " , (*entry.second)->mFile);
		});
		printf( "\n" );

		bool added= false;
		do
		{
			added= false;
			std::multimap< int , std::list< Stream* >::iterator >::iterator st = range.first;
			while ( st != range.second )
			{
				std::list< Stream* >::iterator &st2 = st->second;
				if ( st2 != mStreams.end() )
				{
					Stream *stream = *st2;
					if (stream && ( stream->mPriority == i ) )
					{
						newOrder.push_back( stream );
						*st2 = 0;	// Mark as processed
						st2++;
						added = true;
					}
				}
				st++;
			}
		} while ( added );

		// Now add the streams back again in their new order
		std::list< Stream* >::iterator firstPos = duplicateIDIters[ i ];
		while ( !newOrder.empty() )
		{
			Stream *newStream = newOrder.front();
			*firstPos = newStream;
			firstPos++;
			newOrder.pop_front();
		}

		int a=0;
	}

	printf("Flushing...\n");

	std::for_each( mStreams.begin() , mStreams.end() , [this] (Stream *stream)
	{
		fwrite( &stream->mData[0] , sizeof(char) , stream->mData.size() , mTapeFile );
	});
	sStreamPriority = 0;

	mStreams.clear();
}
