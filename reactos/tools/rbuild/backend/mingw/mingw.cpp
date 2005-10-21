/*
 * Copyright (C) 2005 Casper S. Hornstrup
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include "../../pch.h"

#include "mingw.h"
#include <assert.h>
#ifdef _MSC_VER
#define popen _popen
#define pclose _pclose
#else
#include <dirent.h>
#endif//_MSC_VER
#include "modulehandler.h"

#ifdef WIN32
#define MKDIR(s) mkdir(s)
#else
#define MKDIR(s) mkdir(s, 0755)
#endif

using std::string;
using std::vector;
using std::set;
using std::map;

typedef set<string> set_string;


string
v2s ( const string_list& v, int wrap_at )
{
	if ( !v.size() )
		return "";
	string s;
	int wrap_count = 0;
	for ( size_t i = 0; i < v.size(); i++ )
	{
		if ( !v[i].size() )
			continue;
		if ( wrap_at > 0 && wrap_count++ == wrap_at )
			s += " \\\n\t\t";
		else if ( s.size() )
			s += " ";
		s += v[i];
	}
	return s;
}


Directory::Directory ( const string& name_ )
	: name(name_)
{
}

void
Directory::Add ( const char* subdir )
{
	size_t i;
	string s1 = string ( subdir );
	if ( ( i = s1.find ( '$' ) ) != string::npos )
	{
		throw InvalidOperationException ( __FILE__,
		                                  __LINE__,
		                                  "No environment variables can be used here. Path was %s",
		                                  subdir );
	}

	const char* p = strpbrk ( subdir, "/\\" );
	if ( !p )
		p = subdir + strlen(subdir);
	string s ( subdir, p-subdir );
	if ( subdirs.find(s) == subdirs.end() )
		subdirs[s] = new Directory(s);
	if ( *p && *++p )
		subdirs[s]->Add ( p );
}

bool
Directory::mkdir_p ( const char* path )
{
#ifndef _MSC_VER
	DIR *directory;
	directory = opendir ( path );
	if ( directory != NULL )
	{
		closedir ( directory );
		return false;
	}
#endif//_MSC_VER

	if ( MKDIR ( path ) != 0 )
	{
#ifdef _MSC_VER
		if ( errno == EEXIST )
			return false;
#endif//_MSC_VER
		throw AccessDeniedException ( string ( path ) );
	}
	return true;
}

bool
Directory::CreateDirectory ( string path )
{
	size_t index = 0;
	size_t nextIndex;
	if ( isalpha ( path[0] ) && path[1] == ':' && path[2] == CSEP )
	{
		nextIndex = path.find ( CSEP, 3);
	}
	else
		nextIndex = path.find ( CSEP );

	bool directoryWasCreated = false;
	while ( nextIndex != string::npos )
	{
		nextIndex = path.find ( CSEP, index + 1 );
		directoryWasCreated = mkdir_p ( path.substr ( 0, nextIndex ).c_str () );
		index = nextIndex;
	}
	return directoryWasCreated;
}

string
Directory::ReplaceVariable ( string name,
                             string value,
                             string path )
{
	size_t i = path.find ( name );
	if ( i != string::npos )
		return path.replace ( i, name.length (), value );
	else
		return path;
}

void
Directory::ResolveVariablesInPath ( char* buf,
                                    string path )
{
	string s = ReplaceVariable ( "$(INTERMEDIATE)", Environment::GetIntermediatePath (), path );
	s = ReplaceVariable ( "$(OUTPUT)", Environment::GetOutputPath (), s );
	s = ReplaceVariable ( "$(INSTALL)", Environment::GetInstallPath (), s );
	strcpy ( buf, s.c_str () );
}

void
Directory::GenerateTree ( const string& parent,
                          bool verbose )
{
	string path;

	if ( parent.size () > 0 )
	{
		char buf[256];
		
		path = parent + SSEP + name;
		ResolveVariablesInPath ( buf, path );
		if ( CreateDirectory ( buf ) && verbose )
			printf ( "Created %s\n", buf );
	}
	else
		path = name;

	for ( directory_map::iterator i = subdirs.begin ();
		i != subdirs.end ();
		++i )
	{
		i->second->GenerateTree ( path, verbose );
	}
}

string
Directory::EscapeSpaces ( string path )
{
	string newpath;
	char* p = &path[0];
	while ( *p != 0 )
	{
		if ( *p == ' ' )
			newpath = newpath + "\\ ";
		else
			newpath = newpath + *p;
		*p++;
	}
	return newpath;
}

void
Directory::CreateRule ( FILE* f,
                        const string& parent )
{
	string path;

	if ( parent.size() > 0 )
	{
		string escapedParent = EscapeSpaces ( parent );
		fprintf ( f,
			"%s%c%s: | %s\n",
			escapedParent.c_str (),
			CSEP,
			EscapeSpaces ( name ).c_str (),
			escapedParent.c_str () );

		fprintf ( f,
			"\t$(ECHO_MKDIR)\n" );

		fprintf ( f,
			"\t${mkdir} $@\n" );

		path = parent + SSEP + name;
	}
	else
		path = name;

	for ( directory_map::iterator i = subdirs.begin();
		i != subdirs.end();
		++i )
	{
		i->second->CreateRule ( f, path );
	}
}


static class MingwFactory : public Backend::Factory
{
public:
	MingwFactory() : Factory ( "mingw" ) {}
	Backend* operator() ( Project& project,
	                      Configuration& configuration )
	{
		return new MingwBackend ( project,
		                          configuration );
	}
} factory;


MingwBackend::MingwBackend ( Project& project,
                             Configuration& configuration )
	: Backend ( project, configuration ),
	  intermediateDirectory ( new Directory ("$(INTERMEDIATE)" ) ),
	  outputDirectory ( new Directory ( "$(OUTPUT)" ) ),
	  installDirectory ( new Directory ( "$(INSTALL)" ) )
{
	compilerPrefix = "";
}

MingwBackend::~MingwBackend()
{
	delete intermediateDirectory;
	delete outputDirectory;
	delete installDirectory;
}

string
MingwBackend::AddDirectoryTarget ( const string& directory,
                                   Directory* directoryTree )
{
	if ( directory.length () > 0)
		directoryTree->Add ( directory.c_str() );
	return directoryTree->name;
}

void
MingwBackend::ProcessModules ()
{
	printf ( "Processing modules..." );

	vector<MingwModuleHandler*> v;
	size_t i;
	for ( i = 0; i < ProjectNode.modules.size (); i++ )
	{
		Module& module = *ProjectNode.modules[i];
		if ( !module.enabled )
			continue;
		MingwModuleHandler* h = MingwModuleHandler::InstanciateHandler (
			module,
			this );
		if ( module.host == HostDefault )
		{
			module.host = h->DefaultHost();
			assert ( module.host != HostDefault );
		}
		v.push_back ( h );
	}

	size_t iend = v.size ();

	for ( i = 0; i < iend; i++ )
		v[i]->GenerateObjectMacro();
	fprintf ( fMakefile, "\n" );
	for ( i = 0; i < iend; i++ )
		v[i]->GenerateTargetMacro();
	fprintf ( fMakefile, "\n" );

	GenerateAllTarget ( v );
	GenerateInitTarget ();
	GenerateRegTestsRunTarget ();

	for ( i = 0; i < iend; i++ )
		v[i]->GenerateOtherMacros();

	for ( i = 0; i < iend; i++ )
	{
		MingwModuleHandler& h = *v[i];
		h.GeneratePreconditionDependencies ();
		h.Process ();
		h.GenerateInvocations ();
		h.GenerateCleanTarget ();
		h.GenerateInstallTarget ();
		h.GenerateDependsTarget ();
		delete v[i];
	}

	printf ( "done\n" );
}

void
MingwBackend::Process ()
{
	if ( configuration.CheckDependenciesForModuleOnly )
		CheckAutomaticDependenciesForModuleOnly ();
	else
		ProcessNormal ();
}

void
MingwBackend::CheckAutomaticDependenciesForModuleOnly ()
{
	if ( configuration.AutomaticDependencies )
	{
		Module* module = ProjectNode.LocateModule ( configuration.CheckDependenciesForModuleOnlyModule );
		if ( module == NULL )
		{
			printf ( "Module '%s' does not exist\n",
			        configuration.CheckDependenciesForModuleOnlyModule.c_str () );
			return;
		}
		
		printf ( "Checking automatic dependencies for module '%s'...",
		         module->name.c_str () );
		AutomaticDependency automaticDependency ( ProjectNode );
		automaticDependency.CheckAutomaticDependenciesForModule ( *module,
		                                                          configuration.Verbose );
		printf ( "done\n" );
	}
}

void
MingwBackend::ProcessNormal ()
{
	DetectCompiler ();
	DetectBinutils ();
	DetectNetwideAssembler ();
	DetectPipeSupport ();
	DetectPCHSupport ();
	CreateMakefile ();
	GenerateHeader ();
	GenerateGlobalVariables ();
	GenerateXmlBuildFilesMacro ();
	ProcessModules ();
	GenerateInstallTarget ();
	GenerateTestTarget ();
	GenerateDirectoryTargets ();
	GenerateDirectories ();
	UnpackWineResources ();
	GenerateTestSupportCode ();
	GenerateProxyMakefiles ();
	CheckAutomaticDependencies ();
	CloseMakefile ();
}

void
MingwBackend::CreateMakefile ()
{
	fMakefile = fopen ( ProjectNode.makefile.c_str (), "w" );
	if ( !fMakefile )
		throw AccessDeniedException ( ProjectNode.makefile );
	MingwModuleHandler::SetBackend ( this );
	MingwModuleHandler::SetMakefile ( fMakefile );
	MingwModuleHandler::SetUsePch ( use_pch );
}

void
MingwBackend::CloseMakefile () const
{
	if (fMakefile)
		fclose ( fMakefile );
}

void
MingwBackend::GenerateHeader () const
{
	fprintf ( fMakefile, "# THIS FILE IS AUTOMATICALLY GENERATED, EDIT 'ReactOS.xml' INSTEAD\n\n" );
}

string
MingwBackend::GenerateIncludesAndDefines ( IfableData& data ) const
{
	string includeParameters = MingwModuleHandler::GenerateGccIncludeParametersFromVector ( data.includes );
	string defineParameters = MingwModuleHandler::GenerateGccDefineParametersFromVector ( data.defines );
	return includeParameters + " " + defineParameters;
}

void
MingwBackend::GenerateProjectCFlagsMacro ( const char* assignmentOperation,
                                           IfableData& data ) const
{
	fprintf (
		fMakefile,
		"PROJECT_CFLAGS %s",
		assignmentOperation );
	
	fprintf ( fMakefile,
	          " %s",
	          GenerateIncludesAndDefines ( data ).c_str() );

	fprintf ( fMakefile, "\n" );
}

void
MingwBackend::GenerateGlobalCFlagsAndProperties (
	const char* assignmentOperation,
	IfableData& data ) const
{
	size_t i;

	for ( i = 0; i < data.properties.size(); i++ )
	{
		Property& prop = *data.properties[i];
		fprintf ( fMakefile, "%s := %s\n",
			prop.name.c_str(),
			prop.value.c_str() );
	}

	if ( data.includes.size() || data.defines.size() )
	{
		GenerateProjectCFlagsMacro ( assignmentOperation,
		                             data );
	}

	for ( i = 0; i < data.ifs.size(); i++ )
	{
		If& rIf = *data.ifs[i];
		if ( rIf.data.defines.size()
			|| rIf.data.includes.size()
			|| rIf.data.ifs.size() )
		{
			fprintf (
				fMakefile,
				"ifeq (\"$(%s)\",\"%s\")\n",
				rIf.property.c_str(),
				rIf.value.c_str() );
			GenerateGlobalCFlagsAndProperties (
				"+=",
				rIf.data );
			fprintf (
				fMakefile,
				"endif\n\n" );
		}
	}
}

void
MingwBackend::GenerateProjectGccOptionsMacro ( const char* assignmentOperation,
                                               IfableData& data ) const
{
	size_t i;

	fprintf (
		fMakefile,
		"PROJECT_GCCOPTIONS %s",
		assignmentOperation );
	
	for ( i = 0; i < data.compilerFlags.size(); i++ )
	{
		fprintf (
			fMakefile,
			" %s",
			data.compilerFlags[i]->flag.c_str() );
	}

	fprintf ( fMakefile, "\n" );
}

void
MingwBackend::GenerateProjectGccOptions (
	const char* assignmentOperation,
	IfableData& data ) const
{
	size_t i;

	if ( data.compilerFlags.size() )
	{
		GenerateProjectGccOptionsMacro ( assignmentOperation,
		                                 data );
	}

	for ( i = 0; i < data.ifs.size(); i++ )
	{
		If& rIf = *data.ifs[i];
		if ( rIf.data.compilerFlags.size()
		     || rIf.data.ifs.size() )
		{
			fprintf (
				fMakefile,
				"ifeq (\"$(%s)\",\"%s\")\n",
				rIf.property.c_str(),
				rIf.value.c_str() );
			GenerateProjectGccOptions (
				"+=",
				rIf.data );
			fprintf (
				fMakefile,
				"endif\n\n" );
		}
	}
}

string
MingwBackend::GenerateProjectLFLAGS () const
{
	string lflags;
	for ( size_t i = 0; i < ProjectNode.linkerFlags.size (); i++ )
	{
		LinkerFlag& linkerFlag = *ProjectNode.linkerFlags[i];
		if ( lflags.length () > 0 )
			lflags += " ";
		lflags += linkerFlag.flag;
	}
	return lflags;
}

void
MingwBackend::GenerateGlobalVariables () const
{
	fprintf ( fMakefile,
	          "PREFIX := %s\n",
	          compilerPrefix.c_str () );
	fprintf ( fMakefile,
	          "nasm := %s\n",
	          nasmCommand.c_str () );

	GenerateGlobalCFlagsAndProperties ( "=", ProjectNode.non_if_data );
	GenerateProjectGccOptions ( "=", ProjectNode.non_if_data );

	fprintf ( fMakefile, "PROJECT_RCFLAGS := $(PROJECT_CFLAGS)\n" );
	fprintf ( fMakefile, "PROJECT_WIDLFLAGS := $(PROJECT_CFLAGS)\n" );
	fprintf ( fMakefile, "PROJECT_LFLAGS := %s\n",
	          GenerateProjectLFLAGS ().c_str () );
	fprintf ( fMakefile, "PROJECT_CFLAGS += -Wall\n" );
	fprintf ( fMakefile, "PROJECT_CFLAGS += $(PROJECT_GCCOPTIONS)\n" );
	fprintf ( fMakefile, "\n" );
}

bool
MingwBackend::IncludeInAllTarget ( const Module& module ) const
{
	if ( MingwModuleHandler::ReferenceObjects ( module ) )
		return false;
	if ( module.type == BootSector )
		return false;
	if ( module.type == Iso )
		return false;
	if ( module.type == LiveIso )
		return false;
	if ( module.type == Test )
		return false;
	if ( module.type == Alias )
		return false;
	return true;
}

void
MingwBackend::GenerateAllTarget ( const vector<MingwModuleHandler*>& handlers ) const
{
	fprintf ( fMakefile, "all:" );
	int wrap_count = 0;
	size_t iend = handlers.size ();
	for ( size_t i = 0; i < iend; i++ )
	{
		const Module& module = handlers[i]->module;
		if ( IncludeInAllTarget ( module ) )
		{
			if ( wrap_count++ == 5 )
				fprintf ( fMakefile, " \\\n\t\t" ), wrap_count = 0;
			fprintf ( fMakefile,
			          " %s",
			          GetTargetMacro(module).c_str () );
		}
	}
	fprintf ( fMakefile, "\n\t\n\n" );
}

string
MingwBackend::GetBuildToolDependencies () const
{
	string dependencies;
	for ( size_t i = 0; i < ProjectNode.modules.size (); i++ )
	{
		Module& module = *ProjectNode.modules[i];
		if ( !module.enabled )
			continue;
		if ( module.type == BuildTool )
		{
			if ( dependencies.length () > 0 )
				dependencies += " ";
			dependencies += module.GetDependencyPath ();
		}
	}
	return dependencies;
}

void
MingwBackend::GenerateInitTarget () const
{
	fprintf ( fMakefile,
	          "INIT = %s\n",
	          GetBuildToolDependencies ().c_str () );
	fprintf ( fMakefile, "\n" );
}

void
MingwBackend::GenerateRegTestsRunTarget () const
{
	fprintf ( fMakefile,
	          "REGTESTS_RUN_TARGET = regtests.dll\n" );
	fprintf ( fMakefile,
	          "$(REGTESTS_RUN_TARGET): $(REGTESTS_TARGET)\n" );
	fprintf ( fMakefile,
	          "\t$(cp) $(REGTESTS_TARGET) $(REGTESTS_RUN_TARGET)\n" );
	fprintf ( fMakefile, "\n" );
}

void
MingwBackend::GenerateXmlBuildFilesMacro() const
{
	fprintf ( fMakefile,
	          "XMLBUILDFILES = %s \\\n",
	          ProjectNode.GetProjectFilename ().c_str () );
	string xmlbuildFilenames;
	int numberOfExistingFiles = 0;
	for ( size_t i = 0; i < ProjectNode.xmlbuildfiles.size (); i++ )
	{
		XMLInclude& xmlbuildfile = *ProjectNode.xmlbuildfiles[i];
		if ( !xmlbuildfile.fileExists )
			continue;
		numberOfExistingFiles++;
		if ( xmlbuildFilenames.length () > 0 )
			xmlbuildFilenames += " ";
		xmlbuildFilenames += NormalizeFilename ( xmlbuildfile.topIncludeFilename );
		if ( numberOfExistingFiles % 5 == 4 || i == ProjectNode.xmlbuildfiles.size () - 1 )
		{
			fprintf ( fMakefile,
			          "\t%s",
			          xmlbuildFilenames.c_str ());
			if ( i == ProjectNode.xmlbuildfiles.size () - 1 )
			{
				fprintf ( fMakefile, "\n" );
			}
			else
			{
				fprintf ( fMakefile,
				          " \\\n" );
			}
			xmlbuildFilenames.resize ( 0 );
		}
		numberOfExistingFiles++;
	}
	fprintf ( fMakefile, "\n" );
}

string
MingwBackend::GetBin2ResExecutable ()
{
	return NormalizeFilename ( Environment::GetOutputPath () + SSEP + "tools/bin2res/bin2res" + EXEPOSTFIX );
}

void
MingwBackend::UnpackWineResources ()
{
	printf ( "Unpacking WINE resources..." );
	WineResource wineResource ( ProjectNode,
	                            GetBin2ResExecutable () );
	wineResource.UnpackResources ( configuration.Verbose );
	printf ( "done\n" );
}

void
MingwBackend::GenerateTestSupportCode ()
{
	printf ( "Generating test support code..." );
	TestSupportCode testSupportCode ( ProjectNode );
	testSupportCode.GenerateTestSupportCode ( configuration.Verbose );
	printf ( "done\n" );
}

string
MingwBackend::GetProxyMakefileTree () const
{
	if ( configuration.GenerateProxyMakefilesInSourceTree )
		return "";
	else
		return Environment::GetOutputPath ();
}

void
MingwBackend::GenerateProxyMakefiles ()
{
	printf ( "Generating proxy makefiles..." );
	ProxyMakefile proxyMakefile ( ProjectNode );
	proxyMakefile.GenerateProxyMakefiles ( configuration.Verbose,
	                                       GetProxyMakefileTree () );
	printf ( "done\n" );
}

void
MingwBackend::CheckAutomaticDependencies ()
{
	if ( configuration.AutomaticDependencies )
	{
		printf ( "Checking automatic dependencies..." );
		AutomaticDependency automaticDependency ( ProjectNode );
		automaticDependency.CheckAutomaticDependencies ( configuration.Verbose );
		printf ( "done\n" );
	}
}

bool
MingwBackend::IncludeDirectoryTarget ( const string& directory ) const
{
	if ( directory == "$(INTERMEDIATE)" SSEP "tools")
		return false;
	else
		return true;
}

void
MingwBackend::GenerateDirectories ()
{
	printf ( "Creating directories..." );
	intermediateDirectory->GenerateTree ( "", configuration.Verbose );
	outputDirectory->GenerateTree ( "", configuration.Verbose );
	if ( !configuration.MakeHandlesInstallDirectories )
		installDirectory->GenerateTree ( "", configuration.Verbose );
	printf ( "done\n" );
}

bool
MingwBackend::TryToDetectThisCompiler ( const string& compiler )
{
	string command = ssprintf (
		"%s -v 1>%s 2>%s",
		compiler.c_str (),
		NUL,
		NUL );
	int exitcode = system ( command.c_str () );
	return (exitcode == 0);
}

void
MingwBackend::DetectCompiler ()
{
	printf ( "Detecting compiler..." );

	bool detectedCompiler = false;
	const string& ROS_PREFIXValue = Environment::GetVariable ( "ROS_PREFIX" );
	if ( ROS_PREFIXValue.length () > 0 )
	{
		compilerPrefix = ROS_PREFIXValue;
		compilerCommand = compilerPrefix + "-gcc";
		detectedCompiler = TryToDetectThisCompiler ( compilerCommand );
	}
#if defined(WIN32)
	if ( !detectedCompiler )
	{
		compilerPrefix = "";
		compilerCommand = "gcc";
		detectedCompiler = TryToDetectThisCompiler ( compilerCommand );
	}
#endif
	if ( !detectedCompiler )
	{
		compilerPrefix = "mingw32";
		compilerCommand = compilerPrefix + "-gcc";
		detectedCompiler = TryToDetectThisCompiler ( compilerCommand );
	}
	if ( detectedCompiler )
		printf ( "detected (%s)\n", compilerCommand.c_str () );
	else
		printf ( "not detected\n" );
}

bool
MingwBackend::TryToDetectThisNetwideAssembler ( const string& assembler )
{
	string command = ssprintf (
		"%s -h 1>%s 2>%s",
		assembler.c_str (),
		NUL,
		NUL );
	int exitcode = system ( command.c_str () );
	return (exitcode == 0);
}

bool
MingwBackend::TryToDetectThisBinutils ( const string& binutils )
{
	string command = ssprintf (
		"%s -v 1>%s",
		binutils.c_str (),
		NUL,
		NUL );
	int exitcode = system ( command.c_str () );
	return (exitcode == 0);
}

string
MingwBackend::GetBinutilsVersion ( const string& binutilsCommand )
{
	FILE *fp;
	int ch, i;
	char buffer[81];

	string versionCommand = ssprintf ( "%s -v",
	                                   binutilsCommand.c_str (),
	                                   NUL,
	                                   NUL );
	fp = popen ( versionCommand.c_str () , "r" );
	for( i = 0; 
             ( i < 80 ) && 
                 ( feof ( fp ) == 0 && 
                   ( ( ch = fgetc( fp ) ) != -1 ) ); 
             i++ )
	{
		buffer[i] = (char) ch;
	}
	buffer[i] = '\0';
	pclose ( fp );
	
	char separators[] = " ";
	char *token;
	char *prevtoken = NULL;
	
	token = strtok ( buffer, separators );
	while ( token != NULL )
	{
		prevtoken = token;
		token = strtok ( NULL, separators );
	}
	string version = string ( prevtoken );
	int lastDigit = version.find_last_not_of ( "\t\r\n" );
	if ( lastDigit != -1 )
		return string ( version, 0, lastDigit+1 );
	else
		return version;
}

bool
MingwBackend::IsSupportedBinutilsVersion ( const string& binutilsVersion )
{
	if ( ( ( strcmp ( binutilsVersion.c_str (), "20040902") >= 0 ) &&
	       ( strcmp ( binutilsVersion.c_str (), "20041008") <= 0 ) ) ||
    	       ( strcmp ( binutilsVersion.c_str (), "20031001") < 0 ) )
		return false;
	else
		return true;
}

void
MingwBackend::DetectBinutils ()
{
	printf ( "Detecting binutils..." );

	bool detectedBinutils = false;
	const string& ROS_PREFIXValue = Environment::GetVariable ( "ROS_PREFIX" );
	if ( ROS_PREFIXValue.length () > 0 )
	{
		binutilsPrefix = ROS_PREFIXValue;
		binutilsCommand = binutilsPrefix + "-ld";
		detectedBinutils = TryToDetectThisBinutils ( binutilsCommand );
	}
#if defined(WIN32)
	if ( !detectedBinutils )
	{
		binutilsPrefix = "";
		binutilsCommand = "ld";
		detectedBinutils = TryToDetectThisBinutils ( binutilsCommand );
	}
#endif
	if ( !detectedBinutils )
	{
		binutilsPrefix = "mingw32";
		binutilsCommand = binutilsPrefix + "-ld";
		detectedBinutils = TryToDetectThisBinutils ( binutilsCommand );
	}
	if ( detectedBinutils )
	{
		string binutilsVersion = GetBinutilsVersion ( binutilsCommand );
		if ( IsSupportedBinutilsVersion ( binutilsVersion ) )
			printf ( "detected (%s)\n", binutilsCommand.c_str () );
		else
		{
			printf ( "detected (%s), but with unsupported version (%s)\n",
			         binutilsCommand.c_str (),
			         binutilsVersion.c_str () );
			throw UnsupportedBuildToolException ( binutilsCommand, binutilsVersion );
		}
	}
	else
		printf ( "not detected\n" );
}

void
MingwBackend::DetectNetwideAssembler ()
{
	printf ( "Detecting netwide assembler..." );

	nasmCommand = "nasm";
	bool detectedNasm = TryToDetectThisNetwideAssembler ( nasmCommand );
#if defined(WIN32)
	if ( !detectedNasm )
	{
		nasmCommand = "nasmw";
		detectedNasm = TryToDetectThisNetwideAssembler ( nasmCommand );
	}
#endif
	if ( !detectedNasm )
	{
		nasmCommand = "yasm";
		detectedNasm = TryToDetectThisNetwideAssembler ( nasmCommand );
	}
	if ( detectedNasm )
		printf ( "detected (%s)\n", nasmCommand.c_str () );
	else
		printf ( "not detected\n" );
}

void
MingwBackend::DetectPipeSupport ()
{
	printf ( "Detecting compiler -pipe support..." );

	string pipe_detection = "tools" SSEP "rbuild" SSEP "backend" SSEP "mingw" SSEP "pipe_detection.c";
	string pipe_detectionObjectFilename = ReplaceExtension ( pipe_detection,
	                                                         ".o" );
	string command = ssprintf (
		"%s -pipe -c %s -o %s 1>%s 2>%s",
		compilerCommand.c_str (),
		pipe_detection.c_str (),
		pipe_detectionObjectFilename.c_str (),
		NUL,
		NUL );
	int exitcode = system ( command.c_str () );
	FILE* f = fopen ( pipe_detectionObjectFilename.c_str (), "rb" );
	if ( f )
	{
		usePipe = (exitcode == 0);
		fclose ( f );
		unlink ( pipe_detectionObjectFilename.c_str () );
	}
	else
		usePipe = false;

	if ( usePipe )
		printf ( "detected\n" );
	else
		printf ( "not detected\n" );
}

void
MingwBackend::DetectPCHSupport ()
{
	printf ( "Detecting compiler pre-compiled header support..." );

	string path = "tools" SSEP "rbuild" SSEP "backend" SSEP "mingw" SSEP "pch_detection.h";
	string cmd = ssprintf (
		"%s -c %s 1>%s 2>%s",
		compilerCommand.c_str (),
		path.c_str (),
		NUL,
		NUL );
	system ( cmd.c_str () );
	path += ".gch";

	FILE* f = fopen ( path.c_str (), "rb" );
	if ( f )
	{
		use_pch = true;
		fclose ( f );
		unlink ( path.c_str () );
	}
	else
		use_pch = false;

	if ( use_pch )
		printf ( "detected\n" );
	else
		printf ( "not detected\n" );
}

void
MingwBackend::GetNonModuleInstallTargetFiles (
	vector<string>& out ) const
{
	for ( size_t i = 0; i < ProjectNode.installfiles.size (); i++ )
	{
		const InstallFile& installfile = *ProjectNode.installfiles[i];
		string targetFilenameNoFixup = installfile.base + SSEP + installfile.newname;
		string targetFilename = MingwModuleHandler::PassThruCacheDirectory (
			NormalizeFilename ( targetFilenameNoFixup ),
			installDirectory );
		out.push_back ( targetFilename );
	}
}

void
MingwBackend::GetModuleInstallTargetFiles (
	vector<string>& out ) const
{
	for ( size_t i = 0; i < ProjectNode.modules.size (); i++ )
	{
		const Module& module = *ProjectNode.modules[i];
		if ( !module.enabled )
			continue;
		if ( module.installName.length () > 0 )
		{
			string targetFilenameNoFixup;
			if ( module.installBase.length () > 0 )
				targetFilenameNoFixup = module.installBase + SSEP + module.installName;
			else
				targetFilenameNoFixup = module.installName;
			string targetFilename = MingwModuleHandler::PassThruCacheDirectory (
				NormalizeFilename ( targetFilenameNoFixup ),
				installDirectory );
			out.push_back ( targetFilename );
		}
	}
}

void
MingwBackend::GetInstallTargetFiles (
	vector<string>& out ) const
{
	GetNonModuleInstallTargetFiles ( out );
	GetModuleInstallTargetFiles ( out );
}

void
MingwBackend::OutputInstallTarget ( const string& sourceFilename,
	                            const string& targetFilename,
	                            const string& targetDirectory )
{
	string fullTargetFilename;
	if ( targetDirectory.length () > 0)
		fullTargetFilename = targetDirectory + SSEP + targetFilename;
	else
		fullTargetFilename = targetFilename;
	string normalizedTargetFilename = MingwModuleHandler::PassThruCacheDirectory (
		NormalizeFilename ( fullTargetFilename ),
		installDirectory );
	string normalizedTargetDirectory = MingwModuleHandler::PassThruCacheDirectory (
		NormalizeFilename ( targetDirectory ),
		installDirectory );
	fprintf ( fMakefile,
	          "%s: %s | %s\n",
	          normalizedTargetFilename.c_str (),
	          sourceFilename.c_str (),
	          normalizedTargetDirectory.c_str () );
	fprintf ( fMakefile,
	          "\t$(ECHO_CP)\n" );
	fprintf ( fMakefile,
	          "\t${cp} %s %s 1>$(NUL)\n",
	          sourceFilename.c_str (),
	          normalizedTargetFilename.c_str () );
}

void
MingwBackend::OutputNonModuleInstallTargets ()
{
	for ( size_t i = 0; i < ProjectNode.installfiles.size (); i++ )
	{
		const InstallFile& installfile = *ProjectNode.installfiles[i];
		OutputInstallTarget ( installfile.GetPath (),
		                      installfile.newname,
		                      installfile.base );
	}
}

const Module&
MingwBackend::GetAliasedModuleOrModule ( const Module& module ) const
{
	if ( module.aliasedModuleName.size () > 0 )
	{
		const Module* aliasedModule = ProjectNode.LocateModule ( module.aliasedModuleName );
		assert ( aliasedModule );
		return *aliasedModule;
	}
	else
		return module;
}

void
MingwBackend::OutputModuleInstallTargets ()
{
	for ( size_t i = 0; i < ProjectNode.modules.size (); i++ )
	{
		const Module& module = *ProjectNode.modules[i];
		if ( !module.enabled )
			continue;
		if ( module.installName.length () > 0 )
		{
			const Module& aliasedModule = GetAliasedModuleOrModule ( module );
			string sourceFilename = MingwModuleHandler::PassThruCacheDirectory (
				NormalizeFilename ( aliasedModule.GetPath () ),
				outputDirectory );
			OutputInstallTarget ( sourceFilename,
			                      module.installName,
			                      module.installBase );
		}
	}
}

string
MingwBackend::GetRegistrySourceFiles ()
{
	return "bootdata" SSEP "hivecls.inf "
		"bootdata" SSEP "hivedef.inf "
		"bootdata" SSEP "hiveinst.inf "
		"bootdata" SSEP "hivesft.inf "
		"bootdata" SSEP "hivesys.inf";
}

string
MingwBackend::GetRegistryTargetFiles ()
{
	string system32ConfigDirectory = NormalizeFilename (
		MingwModuleHandler::PassThruCacheDirectory (
		"system32" SSEP "config" SSEP,
		installDirectory ) );
	return system32ConfigDirectory + SSEP "default " +
		system32ConfigDirectory + SSEP "sam " +
		system32ConfigDirectory + SSEP "security " +
		system32ConfigDirectory + SSEP "software " +
		system32ConfigDirectory + SSEP "system";
}

void
MingwBackend::OutputRegistryInstallTarget ()
{
	string system32ConfigDirectory = NormalizeFilename (
		MingwModuleHandler::PassThruCacheDirectory (
		"system32" SSEP "config" SSEP,
		installDirectory ) );

	string registrySourceFiles = GetRegistrySourceFiles ();
	string registryTargetFiles = GetRegistryTargetFiles ();
	fprintf ( fMakefile,
	          "install_registry: %s\n",
	          registryTargetFiles.c_str () );
	fprintf ( fMakefile,
	          "%s: %s %s $(MKHIVE_TARGET)\n",
	          registryTargetFiles.c_str (),
	          registrySourceFiles.c_str (),
	          system32ConfigDirectory.c_str () );
	fprintf ( fMakefile,
	          "\t$(ECHO_MKHIVE)\n" );
	fprintf ( fMakefile,
	          "\t$(MKHIVE_TARGET) bootdata %s bootdata" SSEP "hiveinst.inf\n",
	          system32ConfigDirectory.c_str () );
	fprintf ( fMakefile,
	          "\n" );
}

void
MingwBackend::GenerateInstallTarget ()
{
	vector<string> vInstallTargetFiles;
	GetInstallTargetFiles ( vInstallTargetFiles );
	string installTargetFiles = v2s ( vInstallTargetFiles, 5 );
	string registryTargetFiles = GetRegistryTargetFiles ();

	fprintf ( fMakefile,
	          "install: %s %s\n",
	          installTargetFiles.c_str (),
		  registryTargetFiles.c_str () );
	OutputNonModuleInstallTargets ();
	OutputModuleInstallTargets ();
	OutputRegistryInstallTarget ();
	fprintf ( fMakefile,
	          "\n" );
}

void
MingwBackend::GetModuleTestTargets (
	vector<string>& out ) const
{
	for ( size_t i = 0; i < ProjectNode.modules.size (); i++ )
	{
		const Module& module = *ProjectNode.modules[i];
		if ( !module.enabled )
			continue;
		if ( module.type == Test )
			out.push_back ( module.name );
	}
}

void
MingwBackend::GenerateTestTarget ()
{
	vector<string> vTestTargets;
	GetModuleTestTargets ( vTestTargets );
	string testTargets = v2s ( vTestTargets, 5 );

	fprintf ( fMakefile,
	          "test: %s\n",
		  testTargets.c_str () );
	fprintf ( fMakefile,
	          "\n" );
}

void
MingwBackend::GenerateDirectoryTargets ()
{
	intermediateDirectory->CreateRule ( fMakefile, "" );
	outputDirectory->CreateRule ( fMakefile, "" );
	installDirectory->CreateRule ( fMakefile, "" );
}
