/**
 * Copyright (C) 2014 Roman Hiestand
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 * LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#if defined(_WIN32)
#define HAVE_MODE_T		// Workaround for double defined mode_t on Windows
#endif
#include "config.h"
#include "CommonIncludes.h"

#include "EncFSUtilities.h"

// libencfs
#include "encfs.h"

#include "FileUtils.h"
#include "ConfigReader.h"
#include "FSConfig.h"

#include "DirNode.h"
#include "FileNode.h"
#include "Cipher.h"
#include "StreamNameIO.h"
#include "BlockNameIO.h"
#include "NullNameIO.h"
#include "Context.h"

// boost
#include <boost/locale.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>

// Unfortunately, my version of libencfs is a tiny bit newer than 1.7.4 on Linux.
// This define restores full compatibility
#define EFS_COMPATIBILITY_WORKAROUND 1

EncFSUtilities::EncFSUtilities()
{
}

EncFSUtilities::~EncFSUtilities()
{
}

const int V6SubVersion = 20100713;	// Not optimal just to copy the data, but what can you do?

bool EncFSUtilities::createEncFS(const wxString &encFSPath, const wxString &password,
	const wxString &cipherAlgorithm, long cipherKeySize, long cipherBlockSize,
	const wxString &nameEncoding, long keyDerivationDuration,
	bool perBlockHMAC, bool uniqueIV, bool chainedIV, bool externalIV)
{
	std::string cipherAlgo = cipherAlgorithm.ToStdString();

	boost::shared_ptr<Cipher> cipher = Cipher::New( cipherAlgo, cipherKeySize );
	if(!cipher)
		return false;

	rel::Interface nameIOIface = BlockNameIO::CurrentInterface();
	NameIO::AlgorithmList algorithms = NameIO::GetAlgorithmList();
	NameIO::AlgorithmList::const_iterator it;
	for(it = algorithms.begin(); it != algorithms.end(); ++it)
	{
		if(it->name == nameEncoding.ToStdString())
			nameIOIface = it->iface;
	}

#if defined(EFS_COMPATIBILITY_WORKAROUND)
	if(nameIOIface.current() == 4)
		nameIOIface.current() = 3;
#endif

	int blockMACBytes = 0;
	int blockMACRandBytes = 0;
	if(perBlockHMAC)
		blockMACBytes = 8;
	bool allowHoles = true;
	bool reverseEncryption = false;

	boost::shared_ptr<EncFSConfig> config( new EncFSConfig );

	config->cfgType = Config_V6;
	config->cipherIface = cipher->get_interface();
	config->keySize = cipherKeySize;
	config->blockSize = cipherBlockSize;
	config->nameIface = nameIOIface;
	config->creator = "EncFS " EFS_STRINGIFY(VERSION);
	config->subVersion = V6SubVersion;
	config->blockMACBytes = blockMACBytes;
	config->blockMACRandBytes = blockMACRandBytes;
	config->uniqueIV = uniqueIV;
	config->chainedNameIV = chainedIV;
	config->externalIVChaining = externalIV;
	config->allowHoles = allowHoles;

	config->salt.clear();
	config->kdfIterations = 0; // filled in by keying function
	config->desiredKDFDuration = keyDerivationDuration;

	int encodedKeySize = cipher->encodedKeySize();
	unsigned char *encodedKey = new unsigned char[ encodedKeySize ];
	CipherKey volumeKey = cipher->newRandomKey();

	// get user key and use it to encode volume key
	CipherKey userKey = config->getUserKey(std::string(password.mb_str()), "");

	cipher->writeKey( volumeKey, encodedKey, userKey );
	userKey.reset();

	config->assignKeyData(encodedKey, encodedKeySize);
	delete[] encodedKey;

	if(!volumeKey)
	{
		return false;
	}

	std::string rootDir = EncFSUtilities::wxStringToEncFSPath(encFSPath);
	if(!saveConfig( Config_V6, rootDir, config ))
		return false;

	return true;
}

bool EncFSUtilities::getEncFSInfo(const wxString &encFSPath, EncFSInfo &info)
{
	std::string rootDir = wxStringToEncFSPath(encFSPath);
	boost::shared_ptr<EncFSConfig> config(new EncFSConfig);

	ConfigType type = readConfig( rootDir, config );
	switch(type)
	{
	case Config_None:
		info.configVersionString = wxT("Unable to load or parse config file");
		return false;
	case Config_Prehistoric:
		info.configVersionString = wxT("A really old EncFS filesystem was found. \n")
			wxT("It is not supported in this EncFS build.");
		return false;
	case Config_V3:
		info.configVersionString = wxT("Version 3 configuration; ")
			wxT("created by ") + wxString(config->creator.c_str(), *wxConvCurrent);
		break;
	case Config_V4:
		info.configVersionString = wxT("Version 4 configuration; ")
			wxT("created by ") + wxString(config->creator.c_str(), *wxConvCurrent);
		break;
	case Config_V5:
		info.configVersionString = wxT("Version 5 configuration; ")
			wxT("created by ") + wxString(config->creator.c_str(), *wxConvCurrent)
			+ wxString::Format(wxT(" (revision %i)"), config->subVersion);
		break;
	case Config_V6:
		info.configVersionString = wxT("Version 6 configuration; ")
			wxT("created by ") + wxString(config->creator.c_str(), *wxConvCurrent)
			+ wxString::Format(wxT(" (revision %i)"), config->subVersion);
		break;
	}

	boost::shared_ptr<Cipher> cipher = Cipher::New( config->cipherIface, -1 );
	info.cipherAlgorithm = wxString(config->cipherIface.name().c_str(), *wxConvCurrent);
	if(!cipher)
		info.cipherAlgorithm.Append(wxT(" (NOT supported)"));
	info.cipherKeySize = config->keySize;
	info.cipherBlockSize = config->blockSize;

	// check if we support the filename encoding interface..
	boost::shared_ptr<NameIO> nameCoder = NameIO::New( config->nameIface,
		cipher, CipherKey() );
	info.nameEncoding = wxString(config->nameIface.name().c_str(), *wxConvCurrent);
	if(!nameCoder)
		info.nameEncoding.Append(wxT(" (NOT supported)"));

	info.keyDerivationIterations = config->kdfIterations;
	info.saltSize = config->salt.size();
	info.perBlockHMAC = (config->blockMACBytes > 0);
	info.uniqueIV = config->uniqueIV;
	info.chainedIV = config->chainedNameIV;
	info.externalIV = config->externalIVChaining;
	info.allowHoles = config->allowHoles;

	return true;
}

bool EncFSUtilities::changePassword(const wxString &encFSPath,
	const wxString &oldPassword, const wxString &newPassword, wxString &errorMsg)
{
	std::string rootDir = wxStringToEncFSPath(encFSPath);

	boost::shared_ptr<EncFSConfig> config(new EncFSConfig);
	ConfigType cfgType = readConfig( rootDir, config );
	if(cfgType == Config_None)
	{
		errorMsg = wxT("Unable to load or parse config file");
		return false;
	}

	shared_ptr<Cipher> cipher = Cipher::New( config->cipherIface, config->keySize );
    if(!cipher)
    {
		errorMsg = wxT("Unable to find specified cipher \"");
		errorMsg.Append(wxString(config->cipherIface.name().c_str(), *wxConvCurrent));
		errorMsg.Append(wxT("\""));
		return false;
    }

	CipherKey userKey = config->getUserKey(std::string(oldPassword.mb_str()), "");

	// decode volume key using user key -- at this point we detect an incorrect
	// password if the key checksum does not match (causing readKey to fail).
	CipherKey volumeKey = cipher->readKey( config->getKeyData(), userKey );

	if(!volumeKey)
	{
		errorMsg = wxT("Invalid old password");
		return false;
	}

	userKey.reset();
	// reinitialize salt and iteration count
	config->kdfIterations = 0; // generate new

	userKey = config->getUserKey(std::string(newPassword.mb_str()), "");

	// re-encode the volume key using the new user key and write it out..
	bool isOK = false;
	if(userKey)
	{
		int encodedKeySize = cipher->encodedKeySize();
		unsigned char *keyBuf = new unsigned char[ encodedKeySize ];

		// encode volume key with new user key
		cipher->writeKey( volumeKey, keyBuf, userKey );
		userKey.reset();

		config->assignKeyData( keyBuf, encodedKeySize );
		delete [] keyBuf;

		if(saveConfig( cfgType, rootDir, config ))
		{
			// password modified -- changes volume key of filesystem..
			errorMsg = wxT("Volume Key successfully updated.");
			isOK = true;
		}
		else
		{
			errorMsg = wxT("Error saving modified config file.");
		}
	}
	else
	{
		errorMsg = wxT("Error creating key.");
	}

	volumeKey.reset();

	return isOK;
}

static bool exportFile(const boost::shared_ptr<EncFS_Root> &rootInfo, 
	std::string encfsName, std::string targetName)
{
	boost::shared_ptr<FileNode> node = 
		rootInfo->root->lookupNode( encfsName.c_str(), "EncFSMP" );
	if(!node)
		return false;

	struct stat st;
	if(node->getAttr(&st) != 0)
		return false;

	if(node->open(O_RDONLY) < 0)
		return false;

	int fd = fs_layer::creat(targetName.c_str(), st.st_mode);

	const int blockSize = 512;
	int blocks = (node->getSize() + blockSize-1) / blockSize;
	boost::scoped_array<unsigned char> buf(new unsigned char[blockSize]);

	for(int i = 0; i < blocks; i++)
	{
		int writeRet = 0;
		ssize_t readBytes = node->read(i * blockSize, buf.get(), blockSize);
		if(readBytes > 0)
			writeRet = fs_layer::write(fd, buf.get(), static_cast<unsigned int>(readBytes));

		if(writeRet < 0)
		{
			fs_layer::close(fd);
			return false;
		}
	}
	fs_layer::close(fd);

	return true;
}

static bool exportDir(const boost::shared_ptr<EncFS_Root> &rootInfo, 
	std::string volumeDir, std::string destDir)
{
	// Create destination directory with the same permissions as original
	{
		struct stat st;
		boost::shared_ptr<FileNode> dirNode = 
			rootInfo->root->lookupNode( volumeDir.c_str(), "EncFSMP" );
		if(dirNode->getAttr(&st))
			return false;

		fs_layer::mkdir(destDir.c_str(), st.st_mode);
	}

	// Traverse directory
	DirTraverse dt = rootInfo->root->openDir(volumeDir.c_str());
	if(dt.valid())
	{
		std::string name = dt.nextPlaintextName();
		while(!name.empty())
		{
			// Recurse to subdirectories
			if(name != "." && name != "..")
			{
				std::string plainPath = volumeDir + name;
				std::string cpath = rootInfo->root->cipherPath(plainPath.c_str());
				std::string destName = destDir + name;

				bool retVal = true;
				struct stat stBuf;
				if( !fs_layer::lstat( cpath.c_str(), &stBuf ))
				{
					if( S_ISDIR( stBuf.st_mode ) )
					{
						// Recursive call
						exportDir(rootInfo, (plainPath + '/'), 
							destName + '/');
					}
					else if( S_ISREG( stBuf.st_mode ) )
					{
						retVal = exportFile(rootInfo, plainPath.c_str(), 
							destName.c_str());
					}
				}
				else
				{
					retVal = false;
				}

				if(!retVal)
					return retVal;
			}

			name = dt.nextPlaintextName();
		}
	}
	return true;
}

bool EncFSUtilities::exportEncFS(const wxString &encFSPath,
	const wxString &password, const wxString &exportPath, wxString &errorMsg)
{
	std::string rootDir = wxStringToEncFSPath(encFSPath);
	std::ostringstream ostr;

	shared_ptr<EncFS_Opts> opts( new EncFS_Opts() );
	opts->rootDir = rootDir;
	opts->createIfNotFound = false;
	opts->checkKey = false;
	opts->passwordProgram = std::string(password.mb_str());
	RootPtr rootInfo = initFS( NULL, opts, ostr );

	if(!rootInfo)
	{
		if(ostr.str().length() == 0)
			ostr << "No encrypted filesystem found";

		std::string errMsg1 = ostr.str();
		errorMsg = wxString(errMsg1.c_str(), *wxConvCurrent);
		return false;
	}

	std::string destDir = wxStringToEncFSPath(exportPath);

	return exportDir(rootInfo, "/", destDir);
}

std::string EncFSUtilities::wxStringToEncFSPath(const wxString &path)
{
	std::wstring pathUTF16(path.c_str());
#if defined(EFS_WIN32)
	// Replace all \ with /
	boost::replace_all(pathUTF16, L"\\", L"/");
#endif

	// Make sure path has trailing separator
	wxString separators = wxFileName::GetPathSeparators(wxPATH_NATIVE);
	wchar_t lastChar = pathUTF16[pathUTF16.size() - 1];
	if(separators.Find(lastChar) == wxNOT_FOUND)
		pathUTF16.push_back((wchar_t)(wxFileName::GetPathSeparator(wxPATH_UNIX)));

	std::string pathUTF8 = boost::locale::conv::utf_to_utf<char>(pathUTF16.c_str());

	return pathUTF8;
}

