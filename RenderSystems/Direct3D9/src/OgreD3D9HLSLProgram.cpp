/*
-----------------------------------------------------------------------------
This source file is part of OGRE
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2014 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "OgreD3D9HLSLProgram.h"
#include "OgreGpuProgramManager.h"
#include "OgreStringConverter.h"
#include "OgreD3D9GpuProgram.h"
#include "OgreLogManager.h"

namespace Ogre {
    //-----------------------------------------------------------------------
    D3D9HLSLProgram::CmdTarget D3D9HLSLProgram::msCmdTarget;
    D3D9HLSLProgram::CmdColumnMajorMatrices D3D9HLSLProgram::msCmdColumnMajorMatrices;
    D3D9HLSLProgram::CmdOptimisation D3D9HLSLProgram::msCmdOptimisation;
    D3D9HLSLProgram::CmdMicrocode D3D9HLSLProgram::msCmdMicrocode;
    D3D9HLSLProgram::CmdAssemblerCode D3D9HLSLProgram::msCmdAssemblerCode;
    D3D9HLSLProgram::CmdBackwardsCompatibility D3D9HLSLProgram::msCmdBackwardsCompatibility;

    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::prepareImpl()
    {
        HighLevelGpuProgram::prepareImpl();

        uint32 hash = _getHash();
        if ( GpuProgramManager::getSingleton().isMicrocodeAvailableInCache(hash) )
        {
            getMicrocodeFromCache(hash);
        }
        else
        {
            compileMicrocode();


            if ( GpuProgramManager::getSingleton().getSaveMicrocodesToCache() )
            {
                addMicrocodeToCache(hash);
            }
        }
    }
    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::getMicrocodeFromCache(uint32 id)
    {
        GpuProgramManager::Microcode cacheMicrocode = 
            GpuProgramManager::getSingleton().getMicrocodeFromCache(id);
        
        cacheMicrocode->seek(0);

        // get size the microcode
        size_t microcodeSize = 0;
        cacheMicrocode->read(&microcodeSize, sizeof(size_t));

        // get microcode
        HRESULT hr=D3DXCreateBuffer(microcodeSize, &mMicroCode); 
        cacheMicrocode->read(mMicroCode->GetBufferPointer(), microcodeSize);
        
        // get size of param map
        size_t parametersMapSize = 0;
        cacheMicrocode->read(&parametersMapSize, sizeof(size_t));
                
        // get params
        for(size_t i = 0 ; i < parametersMapSize ; i++)
        {
            String paramName;
            size_t stringSize = 0;
            GpuConstantDefinition def;
            
            // get string size
            cacheMicrocode->read(&stringSize, sizeof(size_t));

            // get string
            paramName.resize(stringSize);
            cacheMicrocode->read(&paramName[0], stringSize);
        
            // get def
            cacheMicrocode->read( &def,  sizeof(GpuConstantDefinition));

            mParametersMap.emplace(paramName, def);
        }
    }
    //-----------------------------------------------------------------------
    const String& D3D9HLSLProgram::getTarget() const
    {
        if(mTarget.empty())
        {
            static String vs_2_0 = "vs_2_0", ps_2_0 = "ps_2_0";
            return mType == GPT_VERTEX_PROGRAM ? vs_2_0 : ps_2_0;
        }

        return mTarget;
    }

    void D3D9HLSLProgram::compileMicrocode(void)
    {
        // Populate preprocessor defines
        String stringBuffer;
        std::vector<D3DXMACRO> defines;
        const D3DXMACRO* pDefines = 0;
        stringBuffer = appendBuiltinDefines(mPreprocessorDefines);

        for(const auto& def : parseDefines(stringBuffer))
        {
            defines.push_back({def.first, def.second});
        }

        // Add NULL terminator
        defines.push_back({0, 0});

        pDefines = &defines[0];

        // Populate compile flags
        DWORD compileFlags = 0;
        if (mColumnMajorMatrices)
            compileFlags |= D3DXSHADER_PACKMATRIX_COLUMNMAJOR;
        else
            compileFlags |= D3DXSHADER_PACKMATRIX_ROWMAJOR;
        if (mBackwardsCompatibility)
            compileFlags |= D3DXSHADER_ENABLE_BACKWARDS_COMPATIBILITY;

#if OGRE_DEBUG_MODE
        compileFlags |= D3DXSHADER_DEBUG;
#endif
        switch (mOptimisationLevel)
        {
        case OPT_DEFAULT:
            compileFlags |= D3DXSHADER_OPTIMIZATION_LEVEL1;
            break;
        case OPT_NONE:
            compileFlags |= D3DXSHADER_SKIPOPTIMIZATION;
            break;
        case OPT_0:
            compileFlags |= D3DXSHADER_OPTIMIZATION_LEVEL0;
            break;
        case OPT_1:
            compileFlags |= D3DXSHADER_OPTIMIZATION_LEVEL1;
            break;
        case OPT_2:
            compileFlags |= D3DXSHADER_OPTIMIZATION_LEVEL2;
            break;
        case OPT_3:
            compileFlags |= D3DXSHADER_OPTIMIZATION_LEVEL3;
            break;
        }


        LPD3DXBUFFER errors = 0;

        // handle includes
        mSource = _resolveIncludes(mSource, this, mFilename, true);

        LPD3DXCONSTANTTABLE pConstTable;

        // Compile & assemble into microcode
        HRESULT hr = D3DXCompileShader(
            mSource.c_str(),
            static_cast<UINT>(mSource.length()),
            pDefines,
            NULL,
            mEntryPoint.c_str(),
            getTarget().c_str(),
            compileFlags,
            &mMicroCode,
            &errors,
            &pConstTable);

        if (FAILED(hr))
        {
            String message = "Cannot assemble D3D9 high-level shader " + mName;
            
            if( errors )
            {
                message += String(" Errors:\n") + static_cast<const char*>(errors->GetBufferPointer());
                errors->Release();
            }

            OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, message);
        }
        else
        {

            // Get contents of the constant table
            D3DXCONSTANTTABLE_DESC desc;
            HRESULT hr = pConstTable->GetDesc(&desc);

            createParameterMappingStructures(true);

            if (FAILED(hr))
            {
                OGRE_EXCEPT(Exception::ERR_INTERNAL_ERROR,
                            "Cannot retrieve constant descriptions from HLSL program");
            }
            // Iterate over the constants
            for (unsigned int i = 0; i < desc.Constants; ++i)
            {
                // Recursively descend through the structure levels
                processParamElement(pConstTable, NULL, "", i);
            }


            SAFE_RELEASE(pConstTable);
        }
    }
    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::addMicrocodeToCache(uint32 id)
    {
        // add to the microcode to the cache
        size_t sizeOfBuffer = sizeof(size_t) + mMicroCode->GetBufferSize() + sizeof(size_t) + mParametersMapSizeAsBuffer;
        
        // create microcode
        GpuProgramManager::Microcode newMicrocode = 
            GpuProgramManager::getSingleton().createMicrocode(sizeOfBuffer);

        // save size of microcode
        size_t microcodeSize = mMicroCode->GetBufferSize();
        newMicrocode->write(&microcodeSize, sizeof(size_t));

        // save microcode
        newMicrocode->write(mMicroCode->GetBufferPointer(), microcodeSize);


        // save size of param map
        size_t parametersMapSize = mParametersMap.size();
        newMicrocode->write(&parametersMapSize, sizeof(size_t));

        // save params
        GpuConstantDefinitionMap::const_iterator iter = mParametersMap.begin();
        GpuConstantDefinitionMap::const_iterator iterE = mParametersMap.end();
        for (; iter != iterE ; ++iter)
        {
            const String & paramName = iter->first;
            const GpuConstantDefinition & def = iter->second;

            // save string size
            size_t stringSize = paramName.size();
            newMicrocode->write(&stringSize, sizeof(size_t));

            // save string
            newMicrocode->write(&paramName[0], stringSize);

            // save def
            newMicrocode->write(&def, sizeof(GpuConstantDefinition));
        }


        // add to the microcode to the cache
        GpuProgramManager::getSingleton().addMicrocodeToCache(id, newMicrocode);
    }
    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::createLowLevelImpl(void)
    {
        if (!mCompileError)
        {
            // Create a low-level program
            mAssemblerProgram = 
                GpuProgramManager::getSingleton().createProgram(
                    mName+"/Delegate",
                    mGroup,
                    getTarget(),
                    mType);
            mAssemblerProgram->setSource("");
            static_cast<D3D9GpuProgram*>(mAssemblerProgram.get())->setExternalMicrocode(mMicroCode);
        }

    }
    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::unloadHighLevelImpl(void)
    {
        mParametersMap.clear();
        mParametersMapSizeAsBuffer = 0;
        SAFE_RELEASE(mMicroCode);
    }
    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::buildConstantDefinitions()
    {
        mConstantDefs->bufferSize = mLogicalToPhysical->bufferSize;

        GpuConstantDefinitionMap::const_iterator iter = mParametersMap.begin();
        GpuConstantDefinitionMap::const_iterator iterE = mParametersMap.end();
        for (; iter != iterE ; ++iter)
        {
            GpuConstantDefinition def = iter->second;

            mConstantDefs->map.emplace(iter->first, iter->second);

            // Record logical / physical mapping
            OGRE_LOCK_MUTEX(mLogicalToPhysical->mutex);
            mLogicalToPhysical->map.emplace(def.logicalIndex,
                    GpuLogicalIndexUse(def.physicalIndex, def.arraySize * def.elementSize, GPV_GLOBAL, def.isFloat() ? BCT_FLOAT : BCT_INT));
            mLogicalToPhysical->bufferSize += def.arraySize * def.elementSize;
        }
        
    }
    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::processParamElement(LPD3DXCONSTANTTABLE pConstTable, D3DXHANDLE parent, String prefix, 
        unsigned int index)
    {
        D3DXHANDLE hConstant = pConstTable->GetConstant(parent, index);

        // Since D3D HLSL doesn't deal with naming of array and struct parameters
        // automatically, we have to do it by hand

        D3DXCONSTANT_DESC desc;
        unsigned int numParams = 1;
        HRESULT hr = pConstTable->GetConstantDesc(hConstant, &desc, &numParams);
        if (FAILED(hr))
        {
            OGRE_EXCEPT(Exception::ERR_INTERNAL_ERROR, 
                "Cannot retrieve constant description from HLSL program.", 
                "D3D9HLSLProgram::processParamElement");
        }

        String paramName = desc.Name;
        // trim the odd '$' which appears at the start of the names in HLSL
        if (paramName.at(0) == '$')
            paramName.erase(paramName.begin());

        // Also trim the '[0]' suffix if it exists, we will add our own indexing later
        if (StringUtil::endsWith(paramName, "[0]", false))
        {
            paramName.erase(paramName.size() - 3);
        }


        if (desc.Class == D3DXPC_STRUCT)
        {
            // work out a new prefix for nested members, if it's an array, we need an index
            prefix = prefix + paramName + ".";
            // Cascade into struct
            for (unsigned int i = 0; i < desc.StructMembers; ++i)
            {
                processParamElement(pConstTable, hConstant, prefix, i);
            }
        }
        else
        {
            // Process params
            if (desc.Type == D3DXPT_FLOAT || desc.Type == D3DXPT_INT || desc.Type == D3DXPT_BOOL)
            {
                size_t paramIndex = desc.RegisterIndex;
                String name = prefix + paramName;
                
                GpuConstantDefinition def;
                def.logicalIndex = paramIndex;
                // populate type, array size & element size
                populateDef(desc, def);
                def.physicalIndex = mLogicalToPhysical->bufferSize*4;
                OGRE_LOCK_MUTEX(mLogicalToPhysical->mutex);
                mLogicalToPhysical->map.emplace(paramIndex,
                    GpuLogicalIndexUse(def.physicalIndex, def.arraySize * def.elementSize, GPV_GLOBAL, def.isFloat() ? BCT_FLOAT : BCT_INT));
                mLogicalToPhysical->bufferSize += def.arraySize * def.elementSize;

                if( mParametersMap.find(name) == mParametersMap.end())
                {
                    mParametersMap.emplace(name, def);
                    mParametersMapSizeAsBuffer += sizeof(size_t);
                    mParametersMapSizeAsBuffer += name.size();
                    mParametersMapSizeAsBuffer += sizeof(GpuConstantDefinition);
                }
                
            }
        }
            
    }
    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::populateDef(D3DXCONSTANT_DESC& d3dDesc, GpuConstantDefinition& def) const
    {
        def.arraySize = d3dDesc.Elements;
        switch(d3dDesc.Type)
        {
        case D3DXPT_INT:
            switch(d3dDesc.Columns)
            {
            case 1:
                def.constType = GCT_INT1;
                break;
            case 2:
                def.constType = GCT_INT2;
                break;
            case 3:
                def.constType = GCT_INT3;
                break;
            case 4:
                def.constType = GCT_INT4;
                break;
            } // columns
            break;
        case D3DXPT_FLOAT:
            switch(d3dDesc.Class)
            {
            case D3DXPC_MATRIX_COLUMNS:
            case D3DXPC_MATRIX_ROWS:
                {
                    int firstDim, secondDim;
                    firstDim = d3dDesc.RegisterCount / d3dDesc.Elements;
                    if (d3dDesc.Class == D3DXPC_MATRIX_ROWS)
                    {
                        secondDim = d3dDesc.Columns;
                    }
                    else
                    {
                        secondDim = d3dDesc.Rows;
                    }
                    switch(firstDim)
                    {
                    case 2:
                        switch(secondDim)
                        {
                        case 2:
                            def.constType = GCT_MATRIX_2X2;
                            def.elementSize = 8; // HLSL always packs
                            break;
                        case 3:
                            def.constType = GCT_MATRIX_2X3;
                            def.elementSize = 8; // HLSL always packs
                            break;
                        case 4:
                            def.constType = GCT_MATRIX_2X4;
                            def.elementSize = 8; 
                            break;
                        } // columns
                        break;
                    case 3:
                        switch(secondDim)
                        {
                        case 2:
                            def.constType = GCT_MATRIX_3X2;
                            def.elementSize = 12; // HLSL always packs
                            break;
                        case 3:
                            def.constType = GCT_MATRIX_3X3;
                            def.elementSize = 12; // HLSL always packs
                            break;
                        case 4:
                            def.constType = GCT_MATRIX_3X4;
                            def.elementSize = 12; 
                            break;
                        } // columns
                        break;
                    case 4:
                        switch(secondDim)
                        {
                        case 2:
                            def.constType = GCT_MATRIX_4X2;
                            def.elementSize = 16; // HLSL always packs
                            break;
                        case 3:
                            def.constType = GCT_MATRIX_4X3;
                            def.elementSize = 16; // HLSL always packs
                            break;
                        case 4:
                            def.constType = GCT_MATRIX_4X4;
                            def.elementSize = 16; 
                            break;
                        } // secondDim
                        break;

                    } // firstDim
                }
                break;
            case D3DXPC_SCALAR:
            case D3DXPC_VECTOR:
                switch(d3dDesc.Columns)
                {
                case 1:
                    def.constType = GCT_FLOAT1;
                    break;
                case 2:
                    def.constType = GCT_FLOAT2;
                    break;
                case 3:
                    def.constType = GCT_FLOAT3;
                    break;
                case 4:
                    def.constType = GCT_FLOAT4;
                    break;
                } // columns
                break;
            }
        default:
            // not mapping samplers, don't need to take the space 
            break;
        };

        // D3D9 pads to 4 elements
        def.elementSize = GpuConstantDefinition::getElementSize(def.constType, true);


    }

    LPD3DXBUFFER D3D9HLSLProgram::getMicroCode()
    {
        return mMicroCode;
    }

    //-----------------------------------------------------------------------
    D3D9HLSLProgram::D3D9HLSLProgram(ResourceManager* creator, const String& name, 
        ResourceHandle handle, const String& group, bool isManual, 
        ManualResourceLoader* loader)
        : HighLevelGpuProgram(creator, name, handle, group, isManual, loader)
        , mColumnMajorMatrices(true)
        , mBackwardsCompatibility(false)
        , mMicroCode(NULL)
        , mOptimisationLevel(OPT_DEFAULT)
        , mParametersMapSizeAsBuffer(0)
    {
        if (createParamDictionary("D3D9HLSLProgram"))
        {
            setupBaseParamDictionary();
            ParamDictionary* dict = getParamDictionary();

            dict->addParameter(ParameterDef("target", 
                "Name of the assembler target to compile down to.",
                PT_STRING),&msCmdTarget);
            dict->addParameter(ParameterDef("column_major_matrices", 
                "Whether matrix packing in column-major order.",
                PT_BOOL),&msCmdColumnMajorMatrices);
            dict->addParameter(ParameterDef("optimisation_level", 
                "The optimisation level to use.",
                PT_STRING),&msCmdOptimisation);
            dict->addParameter(ParameterDef("micro_code", 
                "the micro code.",
                PT_STRING),&msCmdMicrocode);
            dict->addParameter(ParameterDef("assemble_code", 
                "the assemble code.",
                PT_STRING),&msCmdAssemblerCode);
            dict->addParameter(ParameterDef("backwards_compatibility",
                "Enable backwards compatibility mode.",
                PT_BOOL),&msCmdBackwardsCompatibility);
        }
        
    }
    //-----------------------------------------------------------------------
    D3D9HLSLProgram::~D3D9HLSLProgram()
    {
        // have to call this here reather than in Resource destructor
        // since calling virtual methods in base destructors causes crash
        if (isLoaded())
        {
            unload();
        }
        else
        {
            unloadHighLevel();
        }
    }
    //-----------------------------------------------------------------------
    bool D3D9HLSLProgram::isSupported(void) const
    {
        if (mCompileError || !isRequiredCapabilitiesSupported())
            return false;

        return GpuProgramManager::getSingleton().isSyntaxSupported(getTarget());
    }
    //-----------------------------------------------------------------------
    GpuProgramParametersSharedPtr D3D9HLSLProgram::createParameters(void)
    {
        // Call superclass
        GpuProgramParametersSharedPtr params = HighLevelGpuProgram::createParameters();

        // Need to transpose matrices if compiled with column-major matrices
        params->setTransposeMatrices(mColumnMajorMatrices);

        return params;
    }
    //-----------------------------------------------------------------------
    void D3D9HLSLProgram::setTarget(const String& target)
    {
        std::vector<String> profiles = StringUtil::split(target, " ");

        // find first supported format
        for(unsigned int i = 0 ; i < profiles.size() ; i++)
        {
            String & currentProfile = profiles[i];
            if(GpuProgramManager::getSingleton().isSyntaxSupported(currentProfile))
            {
                mTarget = currentProfile;
                return;
            }
        }

        mTarget = profiles.front();
    }

    //-----------------------------------------------------------------------
    const String& D3D9HLSLProgram::getLanguage(void) const
    {
        static const String language = "hlsl";

        return language;
    }
    //-----------------------------------------------------------------------
    //-----------------------------------------------------------------------
    String D3D9HLSLProgram::CmdTarget::doGet(const void *target) const
    {
        return static_cast<const D3D9HLSLProgram*>(target)->getTarget();
    }
    void D3D9HLSLProgram::CmdTarget::doSet(void *target, const String& val)
    {
        static_cast<D3D9HLSLProgram*>(target)->setTarget(val);
    }
    //-----------------------------------------------------------------------
    String D3D9HLSLProgram::CmdColumnMajorMatrices::doGet(const void *target) const
    {
        return StringConverter::toString(static_cast<const D3D9HLSLProgram*>(target)->getColumnMajorMatrices());
    }
    void D3D9HLSLProgram::CmdColumnMajorMatrices::doSet(void *target, const String& val)
    {
        static_cast<D3D9HLSLProgram*>(target)->setColumnMajorMatrices(StringConverter::parseBool(val));
    }
    //-----------------------------------------------------------------------
    String D3D9HLSLProgram::CmdBackwardsCompatibility::doGet(const void *target) const
    {
        return StringConverter::toString(static_cast<const D3D9HLSLProgram*>(target)->getBackwardsCompatibility());
    }
    void D3D9HLSLProgram::CmdBackwardsCompatibility::doSet(void *target, const String& val)
    {
        static_cast<D3D9HLSLProgram*>(target)->setBackwardsCompatibility(StringConverter::parseBool(val));
    }
    //-----------------------------------------------------------------------
    String D3D9HLSLProgram::CmdOptimisation::doGet(const void *target) const
    {
        switch(static_cast<const D3D9HLSLProgram*>(target)->getOptimisationLevel())
        {
        default:
        case OPT_DEFAULT:
            return "default";
        case OPT_NONE:
            return "none";
        case OPT_0:
            return "0";
        case OPT_1:
            return "1";
        case OPT_2:
            return "2";
        case OPT_3:
            return "3";
        }
    }
    void D3D9HLSLProgram::CmdOptimisation::doSet(void *target, const String& val)
    {
        if (StringUtil::startsWith(val, "default", true))
            static_cast<D3D9HLSLProgram*>(target)->setOptimisationLevel(OPT_DEFAULT);
        else if (StringUtil::startsWith(val, "none", true))
            static_cast<D3D9HLSLProgram*>(target)->setOptimisationLevel(OPT_NONE);
        else if (StringUtil::startsWith(val, "0", true))
            static_cast<D3D9HLSLProgram*>(target)->setOptimisationLevel(OPT_0);
        else if (StringUtil::startsWith(val, "1", true))
            static_cast<D3D9HLSLProgram*>(target)->setOptimisationLevel(OPT_1);
        else if (StringUtil::startsWith(val, "2", true))
            static_cast<D3D9HLSLProgram*>(target)->setOptimisationLevel(OPT_2);
        else if (StringUtil::startsWith(val, "3", true))
            static_cast<D3D9HLSLProgram*>(target)->setOptimisationLevel(OPT_3);
    }

    //-----------------------------------------------------------------------
    String D3D9HLSLProgram::CmdMicrocode::doGet(const void *target) const
    {
        D3D9HLSLProgram* program=const_cast<D3D9HLSLProgram*>(static_cast<const D3D9HLSLProgram*>(target));
        LPD3DXBUFFER buffer=program->getMicroCode();
        if(buffer)
        {
            char* str  =static_cast<Ogre::String::value_type*>(buffer->GetBufferPointer());
            size_t size=static_cast<size_t>(buffer->GetBufferSize());
            Ogre::String code;
            code.assign(str,size);
            return code;
        }
        else
        {
            return String();
        }
    }
    void D3D9HLSLProgram::CmdMicrocode::doSet(void *target, const String& val)
    {
        //nothing to do 
        //static_cast<D3D9HLSLProgram*>(target)->setColumnMajorMatrices(StringConverter::parseBool(val));
    }
    //-----------------------------------------------------------------------
    String D3D9HLSLProgram::CmdAssemblerCode::doGet(const void *target) const
    {
        D3D9HLSLProgram* program=const_cast<D3D9HLSLProgram*>(static_cast<const D3D9HLSLProgram*>(target));
        LPD3DXBUFFER buffer=program->getMicroCode();
        if(buffer)
        {
            CONST DWORD* code =static_cast<CONST DWORD*>(buffer->GetBufferPointer());
            LPD3DXBUFFER pDisassembly=0;
            HRESULT hr=D3DXDisassembleShader(code,FALSE,"// assemble code from D3D9HLSLProgram\n",&pDisassembly);
            if(pDisassembly)
            {
                char* str  =static_cast<Ogre::String::value_type*>(pDisassembly->GetBufferPointer());
                size_t size=static_cast<size_t>(pDisassembly->GetBufferSize());
                Ogre::String assemble_code;
                assemble_code.assign(str,size);
                pDisassembly->Release();
                return assemble_code;
            }
            return String();
        }
        else
        {
            return String();
        }
    }
    void D3D9HLSLProgram::CmdAssemblerCode::doSet(void *target, const String& val)
    {
        //nothing to do 
        //static_cast<D3D9HLSLProgram*>(target)->setColumnMajorMatrices(StringConverter::parseBool(val));
    }
}
