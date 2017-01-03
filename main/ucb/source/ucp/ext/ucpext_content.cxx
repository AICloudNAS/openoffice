/**************************************************************
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 * 
 *************************************************************/



#include "precompiled_ext.hxx"

#include "ucpext_content.hxx"

#include "ucpext_content.hxx"
#include "ucpext_provider.hxx"
#include "ucpext_resultset.hxx"

/** === begin UNO includes === **/
#include <com/sun/star/beans/PropertyAttribute.hpp>
#include <com/sun/star/beans/XPropertyAccess.hpp>
#include <com/sun/star/lang/IllegalAccessException.hpp>
#include <com/sun/star/sdbc/XRow.hpp>
#include <com/sun/star/ucb/XCommandInfo.hpp>
#include <com/sun/star/ucb/XPersistentPropertySet.hpp>
#include <com/sun/star/io/XOutputStream.hpp>
#include <com/sun/star/io/XActiveDataSink.hpp>
#include <com/sun/star/ucb/OpenCommandArgument2.hpp>
#include <com/sun/star/ucb/OpenMode.hpp>
#include <com/sun/star/ucb/UnsupportedDataSinkException.hpp>
#include <com/sun/star/ucb/UnsupportedOpenModeException.hpp>
#include <com/sun/star/ucb/OpenCommandArgument2.hpp>
#include <com/sun/star/ucb/OpenMode.hpp>
#include <com/sun/star/ucb/XDynamicResultSet.hpp>
#include <com/sun/star/lang/IllegalAccessException.hpp>
#include <com/sun/star/deployment/XPackageInformationProvider.hpp>
/** === end UNO includes === **/

#include <ucbhelper/contentidentifier.hxx>
#include <ucbhelper/propertyvalueset.hxx>
#include <ucbhelper/cancelcommandexecution.hxx>
#include <ucbhelper/content.hxx>
#include <tools/diagnose_ex.h>
#include <comphelper/string.hxx>
#include <comphelper/componentcontext.hxx>
#include <rtl/ustrbuf.hxx>
#include <rtl/uri.hxx>

#include <algorithm>

//......................................................................................................................
namespace ucb { namespace ucp { namespace ext
{
//......................................................................................................................

	/** === begin UNO using === **/
	using ::com::sun::star::uno::Reference;
	using ::com::sun::star::uno::XInterface;
	using ::com::sun::star::uno::UNO_QUERY;
	using ::com::sun::star::uno::UNO_QUERY_THROW;
	using ::com::sun::star::uno::UNO_SET_THROW;
	using ::com::sun::star::uno::Exception;
	using ::com::sun::star::uno::RuntimeException;
	using ::com::sun::star::uno::Any;
	using ::com::sun::star::uno::makeAny;
	using ::com::sun::star::uno::Sequence;
	using ::com::sun::star::uno::Type;
    using ::com::sun::star::lang::XMultiServiceFactory;
    using ::com::sun::star::ucb::XContentIdentifier;
    using ::com::sun::star::ucb::IllegalIdentifierException;
    using ::com::sun::star::ucb::XContent;
    using ::com::sun::star::ucb::XCommandEnvironment;
    using ::com::sun::star::ucb::Command;
    using ::com::sun::star::ucb::CommandAbortedException;
    using ::com::sun::star::beans::Property;
    using ::com::sun::star::lang::IllegalArgumentException;
    using ::com::sun::star::beans::PropertyValue;
    using ::com::sun::star::ucb::OpenCommandArgument2;
    using ::com::sun::star::ucb::XDynamicResultSet;
    using ::com::sun::star::ucb::UnsupportedOpenModeException;
    using ::com::sun::star::io::XOutputStream;
    using ::com::sun::star::io::XActiveDataSink;
    using ::com::sun::star::io::XInputStream;
    using ::com::sun::star::ucb::UnsupportedDataSinkException;
    using ::com::sun::star::ucb::UnsupportedCommandException;
    using ::com::sun::star::sdbc::XRow;
    using ::com::sun::star::beans::XPropertySet;
    using ::com::sun::star::beans::PropertyChangeEvent;
    using ::com::sun::star::lang::IllegalAccessException;
    using ::com::sun::star::ucb::CommandInfo;
    using ::com::sun::star::deployment::XPackageInformationProvider;
	/** === end UNO using === **/
    namespace OpenMode = ::com::sun::star::ucb::OpenMode;
    namespace PropertyAttribute = ::com::sun::star::beans::PropertyAttribute;

    //==================================================================================================================
    //= helper
    //==================================================================================================================
    namespace
    {
        //--------------------------------------------------------------------------------------------------------------
        ::rtl::OUString lcl_compose( const ::rtl::OUString& i_rBaseURL, const ::rtl::OUString& i_rRelativeURL )
        {
            ENSURE_OR_RETURN( i_rBaseURL.getLength(), "illegal base URL", i_rRelativeURL );

            ::rtl::OUStringBuffer aComposer( i_rBaseURL );
            if ( i_rBaseURL.getStr()[ i_rBaseURL.getLength() - 1 ] != '/' )
                aComposer.append( sal_Unicode( '/' ) );
            aComposer.append( i_rRelativeURL );
            return aComposer.makeStringAndClear();
        }

        //--------------------------------------------------------------------------------------------------------------
        struct SelectPropertyName : public ::std::unary_function< Property, ::rtl::OUString >
        {
            const ::rtl::OUString& operator()( const Property& i_rProperty ) const
            {
                return i_rProperty.Name;
            }
        };
    }

    //==================================================================================================================
    //= Content
    //==================================================================================================================
    //------------------------------------------------------------------------------------------------------------------
    Content::Content( const Reference< XMultiServiceFactory >& i_rORB, ::ucbhelper::ContentProviderImplHelper* i_pProvider,
                      const Reference< XContentIdentifier >& i_rIdentifier )
        :Content_Base( i_rORB, i_pProvider, i_rIdentifier )
        ,m_eExtContentType( E_UNKNOWN )
        ,m_aIsFolder()
        ,m_aContentType()
        ,m_sExtensionId()
        ,m_sPathIntoExtension()
    {
        const ::rtl::OUString sURL( getIdentifier()->getContentIdentifier() );
        if ( denotesRootContent( sURL ) )
        {
            m_eExtContentType = E_ROOT;
        }
        else
        {
            const ::rtl::OUString sRelativeURL( sURL.copy( ContentProvider::getRootURL().getLength() ) );
            const sal_Int32 nSepPos = sRelativeURL.indexOf( '/' );
            if ( ( nSepPos == -1 ) || ( nSepPos == sRelativeURL.getLength() - 1 ) )
            {
                m_eExtContentType = E_EXTENSION_ROOT;
            }
            else
            {
                m_eExtContentType = E_EXTENSION_CONTENT;
            }
        }

        if ( m_eExtContentType != E_ROOT )
        {
            const ::rtl::OUString sRootURL = ContentProvider::getRootURL();
            m_sExtensionId = sURL.copy( sRootURL.getLength() );

            const sal_Int32 nNextSep = m_sExtensionId.indexOf( '/' );
            if ( nNextSep > -1 )
            {
                m_sPathIntoExtension = m_sExtensionId.copy( nNextSep + 1 );
                m_sExtensionId = m_sExtensionId.copy( 0, nNextSep );
            }
            m_sExtensionId = Content::decodeIdentifier( m_sExtensionId );
        }
    }

    //------------------------------------------------------------------------------------------------------------------
    Content::~Content()
    {
    }

    //------------------------------------------------------------------------------------------------------------------
    ::rtl::OUString SAL_CALL Content::getImplementationName() throw( RuntimeException )
    {
        return ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "org.openoffice.comp.ucp.ext.Content" ) );
    }

    //------------------------------------------------------------------------------------------------------------------
    Sequence< ::rtl::OUString > SAL_CALL Content::getSupportedServiceNames() throw( RuntimeException )
    {
        Sequence< ::rtl::OUString > aServiceNames(2);
        aServiceNames[0] = ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "com.sun.star.ucb.Content" ) );
        aServiceNames[1] = ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "com.sun.star.ucb.ExtensionContent" ) );
        return aServiceNames;
    }

    //------------------------------------------------------------------------------------------------------------------
    ::rtl::OUString SAL_CALL Content::getContentType() throw( RuntimeException )
    {
        impl_determineContentType();
        return *m_aContentType;
    }

    //------------------------------------------------------------------------------------------------------------------
    Any SAL_CALL Content::execute( const Command& aCommand, sal_Int32 /* CommandId */, const Reference< XCommandEnvironment >& i_rEvironment )
        throw( Exception, CommandAbortedException, RuntimeException )
    {
        Any aRet;

        if ( aCommand.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "getPropertyValues" ) ) )
	    {
            Sequence< Property > Properties;
		    if ( !( aCommand.Argument >>= Properties ) )
		    {
                ::ucbhelper::cancelCommandExecution( makeAny( IllegalArgumentException(
                    ::rtl::OUString(), *this, -1 ) ),
                    i_rEvironment );
                // unreachable
		    }

            aRet <<= getPropertyValues( Properties, i_rEvironment );
	    }
        else if ( aCommand.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "setPropertyValues" ) ) )
        {
            Sequence< PropertyValue > aProperties;
		    if ( !( aCommand.Argument >>= aProperties ) )
		    {
                ::ucbhelper::cancelCommandExecution( makeAny( IllegalArgumentException(
                    ::rtl::OUString(), *this, -1 ) ),
                    i_rEvironment );
                // unreachable
            }

		    if ( !aProperties.getLength() )
		    {
                ::ucbhelper::cancelCommandExecution( makeAny( IllegalArgumentException(
                    ::rtl::OUString(), *this, -1 ) ),
                    i_rEvironment );
                // unreachable
            }

            aRet <<= setPropertyValues( aProperties, i_rEvironment );
	    }
        else if ( aCommand.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "getPropertySetInfo" ) ) )
        {
		    // implemented by base class.
		    aRet <<= getPropertySetInfo( i_rEvironment );
	    }
        else if ( aCommand.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "getCommandInfo" ) ) )
        {
		    // implemented by base class.
		    aRet <<= getCommandInfo( i_rEvironment );
	    }
        else if ( aCommand.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "open" ) ) )
        {
            OpenCommandArgument2 aOpenCommand;
      	    if ( !( aCommand.Argument >>= aOpenCommand ) )
		    {
                ::ucbhelper::cancelCommandExecution( makeAny( IllegalArgumentException(
                    ::rtl::OUString(), *this, -1 ) ),
                    i_rEvironment );
                // unreachable
            }

            sal_Bool bOpenFolder =
                ( ( aOpenCommand.Mode == OpenMode::ALL ) ||
                  ( aOpenCommand.Mode == OpenMode::FOLDERS ) ||
                  ( aOpenCommand.Mode == OpenMode::DOCUMENTS ) );


            if ( bOpenFolder && impl_isFolder() )
		    {
                Reference< XDynamicResultSet > xSet = new ResultSet(
                    m_xSMgr, this, aOpenCommand, i_rEvironment );
    		    aRet <<= xSet;
  		    }

            if ( aOpenCommand.Sink.is() )
            {
                const ::rtl::OUString sPhysicalContentURL( getPhysicalURL() );
                ::ucbhelper::Content aRequestedContent( sPhysicalContentURL, i_rEvironment );
                aRet = aRequestedContent.executeCommand( ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "open" ) ), makeAny( aOpenCommand ) );
		    }
	    }

	    else
	    {
            ::ucbhelper::cancelCommandExecution( makeAny( UnsupportedCommandException(
                ::rtl::OUString(), *this ) ),
                i_rEvironment );
            // unreachable
        }

	    return aRet;
    }

    //------------------------------------------------------------------------------------------------------------------
    void SAL_CALL Content::abort( sal_Int32 ) throw( RuntimeException )
    {
    }

    //------------------------------------------------------------------------------------------------------------------
    ::rtl::OUString Content::encodeIdentifier( const ::rtl::OUString& i_rIdentifier )
    {
        return ::rtl::Uri::encode( i_rIdentifier, rtl_UriCharClassRegName, rtl_UriEncodeIgnoreEscapes,
            RTL_TEXTENCODING_UTF8 );
    }

    //------------------------------------------------------------------------------------------------------------------
    ::rtl::OUString Content::decodeIdentifier( const ::rtl::OUString& i_rIdentifier )
    {
        return ::rtl::Uri::decode( i_rIdentifier, rtl_UriDecodeWithCharset, RTL_TEXTENCODING_UTF8 );
    }

    //------------------------------------------------------------------------------------------------------------------
    bool Content::denotesRootContent( const ::rtl::OUString& i_rContentIdentifier )
    {
        const ::rtl::OUString sRootURL( ContentProvider::getRootURL() );
        if ( i_rContentIdentifier == sRootURL )
            return true;

        // the root URL contains only two trailing /, but we also recognize 3 of them as denoting the root URL
        if  (   i_rContentIdentifier.match( sRootURL )
            &&  ( i_rContentIdentifier.getLength() == sRootURL.getLength() + 1 )
            &&  ( i_rContentIdentifier[ i_rContentIdentifier.getLength() - 1 ] == '/' )
            )
            return true;

        return false;
    }

    //------------------------------------------------------------------------------------------------------------------
    ::rtl::OUString Content::getParentURL()
    {
        const ::rtl::OUString sRootURL( ContentProvider::getRootURL() );

        switch ( m_eExtContentType )
        {
        case E_ROOT:
            // don't have a parent
            return sRootURL;

        case E_EXTENSION_ROOT:
            // our parent is the root itself
            return sRootURL;

        case E_EXTENSION_CONTENT:
        {
            const ::rtl::OUString sURL = m_xIdentifier->getContentIdentifier();

            // cut the root URL
            ENSURE_OR_BREAK( sURL.match( sRootURL, 0 ), "illegal URL structure - no root" );
            ::rtl::OUString sRelativeURL( sURL.copy( sRootURL.getLength() ) );

            // cut the extension ID
            const ::rtl::OUString sSeparatedExtensionId( encodeIdentifier( m_sExtensionId ) + ::rtl::OUString( sal_Unicode( '/' ) ) );
            ENSURE_OR_BREAK( sRelativeURL.match( sSeparatedExtensionId ), "illegal URL structure - no extension ID" );
            sRelativeURL = sRelativeURL.copy( sSeparatedExtensionId.getLength() );

            // cut the final slash (if any)
            ENSURE_OR_BREAK( sRelativeURL.getLength(), "illegal URL structure - ExtensionContent should have a level below the extension ID" );
            if ( sRelativeURL.getStr()[ sRelativeURL.getLength() - 1 ] == '/' )
                sRelativeURL = sRelativeURL.copy( 0, sRelativeURL.getLength() - 1 );

            // remove the last segment
            const sal_Int32 nLastSep = sRelativeURL.lastIndexOf( '/' );
            sRelativeURL = sRelativeURL.copy( 0, nLastSep != -1 ? nLastSep : 0 );

            ::rtl::OUStringBuffer aComposer;
            aComposer.append( sRootURL );
            aComposer.append( sSeparatedExtensionId );
            aComposer.append( sRelativeURL );
            return aComposer.makeStringAndClear();
        }

        default:
            OSL_ENSURE( false, "Content::getParentURL: unhandled case!" );
            break;
        }
        return ::rtl::OUString();
    }

    //------------------------------------------------------------------------------------------------------------------
    Reference< XRow > Content::getArtificialNodePropertyValues( const Reference< XMultiServiceFactory >& i_rORB,
        const Sequence< Property >& i_rProperties, const ::rtl::OUString& i_rTitle )
    {
	    // note: empty sequence means "get values of all supported properties".
        ::rtl::Reference< ::ucbhelper::PropertyValueSet > xRow = new ::ucbhelper::PropertyValueSet( i_rORB );

	    const sal_Int32 nCount = i_rProperties.getLength();
	    if ( nCount )
	    {
            Reference< XPropertySet > xAdditionalPropSet;

            const Property* pProps = i_rProperties.getConstArray();
		    for ( sal_Int32 n = 0; n < nCount; ++n )
		    {
                const Property& rProp = pProps[ n ];

			    // Process Core properties.
                if ( rProp.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "ContentType" ) ) )
                {
                    xRow->appendString ( rProp, ContentProvider::getArtificialNodeContentType() );
			    }
                else if ( rProp.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "Title" ) ) )
			    {
				    xRow->appendString ( rProp, i_rTitle );
			    }
                else if ( rProp.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "IsDocument" ) ) )
			    {
				    xRow->appendBoolean( rProp, sal_False );
			    }
                else if ( rProp.Name.equalsAsciiL( RTL_CONSTASCII_STRINGPARAM( "IsFolder" ) ) )
			    {
				    xRow->appendBoolean( rProp, sal_True );
			    }
			    else
			    {
				    // append empty entry.
				    xRow->appendVoid( rProp );
			    }
		    }
	    }
	    else
	    {
		    // Append all Core Properties.
		    xRow->appendString ( Property( ::rtl::OUString::createFromAscii( "ContentType" ),
					      -1,
                          getCppuType( static_cast< const ::rtl::OUString * >( 0 ) ),
                          PropertyAttribute::BOUND | PropertyAttribute::READONLY ),
			    ContentProvider::getArtificialNodeContentType() );
		    xRow->appendString ( Property( ::rtl::OUString::createFromAscii( "Title" ),
					      -1,
                          getCppuType( static_cast< const ::rtl::OUString * >( 0 ) ),
                          PropertyAttribute::BOUND | PropertyAttribute::READONLY ),
			    i_rTitle );
		    xRow->appendBoolean( Property( ::rtl::OUString::createFromAscii( "IsDocument" ),
					      -1,
					      getCppuBooleanType(),
                          PropertyAttribute::BOUND | PropertyAttribute::READONLY ),
			    sal_False );
		    xRow->appendBoolean( Property( ::rtl::OUString::createFromAscii( "IsFolder" ),
					      -1,
					      getCppuBooleanType(),
                          PropertyAttribute::BOUND | PropertyAttribute::READONLY ),
			    sal_True );
	    }

        return Reference< XRow >( xRow.get() );
    }

    //------------------------------------------------------------------------------------------------------------------
    ::rtl::OUString Content::getPhysicalURL() const
    {
        ENSURE_OR_RETURN( m_eExtContentType != E_ROOT, "illegal call", ::rtl::OUString() );

        // create an ucb::XContent for the physical file within the deployed extension
        const ::comphelper::ComponentContext aContext( m_xSMgr );
        const Reference< XPackageInformationProvider > xPackageInfo(
            aContext.getSingleton( "com.sun.star.deployment.PackageInformationProvider" ), UNO_QUERY_THROW );
        const ::rtl::OUString sPackageLocation( xPackageInfo->getPackageLocation( m_sExtensionId ) );

        if ( m_sPathIntoExtension.getLength() == 0 )
            return sPackageLocation;
        return lcl_compose( sPackageLocation, m_sPathIntoExtension );
    }

    //------------------------------------------------------------------------------------------------------------------
    Reference< XRow > Content::getPropertyValues( const Sequence< Property >& i_rProperties, const Reference< XCommandEnvironment >& i_rEnv )
    {
        ::osl::Guard< ::osl::Mutex > aGuard( m_aMutex );

        switch ( m_eExtContentType )
        {
        case E_ROOT:
	        return getArtificialNodePropertyValues( m_xSMgr, i_rProperties, ContentProvider::getRootURL() );
        case E_EXTENSION_ROOT:
	        return getArtificialNodePropertyValues( m_xSMgr, i_rProperties, m_sExtensionId );
        case E_EXTENSION_CONTENT:
        {
            const ::rtl::OUString sPhysicalContentURL( getPhysicalURL() );
            ::ucbhelper::Content aRequestedContent( sPhysicalContentURL, i_rEnv );

            // translate the property request
            Sequence< ::rtl::OUString > aPropertyNames( i_rProperties.getLength() );
            ::std::transform(
                i_rProperties.getConstArray(),
                i_rProperties.getConstArray() + i_rProperties.getLength(),
                aPropertyNames.getArray(),
                SelectPropertyName()
            );
            const Sequence< Any > aPropertyValues = aRequestedContent.getPropertyValues( aPropertyNames );
            const ::rtl::Reference< ::ucbhelper::PropertyValueSet > xValueRow = new ::ucbhelper::PropertyValueSet( m_xSMgr );
            sal_Int32 i=0;
            for (   const Any* value = aPropertyValues.getConstArray();
                    value != aPropertyValues.getConstArray() + aPropertyValues.getLength();
                    ++value, ++i
                )
            {
                xValueRow->appendObject( aPropertyNames[i], *value );
            }
            return xValueRow.get();
        }

        default:
            OSL_ENSURE( false, "Content::getPropertyValues: unhandled case!" );
            break;
        }

        OSL_ENSURE( false, "Content::getPropertyValues: unreachable!" );
        return NULL;
    }

    //------------------------------------------------------------------------------------------------------------------
    Sequence< Any > Content::setPropertyValues( const Sequence< PropertyValue >& i_rValues, const Reference< XCommandEnvironment >& /* xEnv */)
    {
        ::osl::ClearableGuard< osl::Mutex > aGuard( m_aMutex );

        Sequence< Any > aRet( i_rValues.getLength() );
        Sequence< PropertyChangeEvent > aChanges( i_rValues.getLength() );

        PropertyChangeEvent aEvent;
        aEvent.Source         = static_cast< cppu::OWeakObject * >( this );
	    aEvent.Further 		  = sal_False;
        aEvent.PropertyHandle = -1;

        const PropertyValue* pValues = i_rValues.getConstArray();
	    const sal_Int32 nCount = i_rValues.getLength();

	    for ( sal_Int32 n = 0; n < nCount; ++n, ++pValues )
	    {
		    // all our properties are read-only ...
            aRet[ n ] <<= IllegalAccessException( ::rtl::OUString::createFromAscii( "property is read-only." ), *this );
	    }

        return aRet;
    }

    //------------------------------------------------------------------------------------------------------------------
    Sequence< CommandInfo > Content::getCommands( const Reference< XCommandEnvironment > & /*xEnv*/ )
    {
	    sal_uInt32 nCommandCount = 5;
        static const CommandInfo aCommandInfoTable[] =
	    {
		    ///////////////////////////////////////////////////////////////
		    // Mandatory commands
		    ///////////////////////////////////////////////////////////////
            CommandInfo(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "getCommandInfo" ) ),
			    -1,
			    getCppuVoidType()
		    ),
            CommandInfo(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "getPropertySetInfo" ) ),
			    -1,
			    getCppuVoidType()
		    ),
            CommandInfo(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "getPropertyValues" ) ),
			    -1,
                getCppuType(
                    static_cast< Sequence< Property > * >( 0 ) )
		    ),
            CommandInfo(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "setPropertyValues" ) ),
			    -1,
                getCppuType(
                    static_cast< Sequence< PropertyValue > * >( 0 ) )
		    )
		    ///////////////////////////////////////////////////////////////
		    // Optional standard commands
		    ///////////////////////////////////////////////////////////////
            , CommandInfo(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "open" ) ),
			    -1,
                getCppuType( static_cast< OpenCommandArgument2 * >( 0 ) )
		    )
	    };

        return Sequence< CommandInfo >( aCommandInfoTable, nCommandCount );
    }

    //------------------------------------------------------------------------------------------------------------------
    Sequence< Property > Content::getProperties( const Reference< XCommandEnvironment > & /*xEnv*/ )
    {
        static Property aProperties[] =
	    {
            Property(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "ContentType" ) ),
			    -1,
                getCppuType( static_cast< const ::rtl::OUString * >( 0 ) ),
                PropertyAttribute::BOUND | PropertyAttribute::READONLY
		    ),
            Property(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "IsDocument" ) ),
			    -1,
			    getCppuBooleanType(),
                PropertyAttribute::BOUND | PropertyAttribute::READONLY
		    ),
            Property(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "IsFolder" ) ),
			    -1,
			    getCppuBooleanType(),
                PropertyAttribute::BOUND | PropertyAttribute::READONLY
		    ),
            Property(
                ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "Title" ) ),
			    -1,
                getCppuType( static_cast< const ::rtl::OUString * >( 0 ) ),
                PropertyAttribute::BOUND | PropertyAttribute::READONLY
		    )
	    };
        return Sequence< Property >( aProperties, sizeof( aProperties ) / sizeof( aProperties[0] ) );
    }

    //------------------------------------------------------------------------------------------------------------------
    bool Content::impl_isFolder()
    {
        if ( !!m_aIsFolder )
            return *m_aIsFolder;

        bool bIsFolder = false;
        try
        {
            Sequence< Property > aProps(1);
            aProps[0].Name = ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "IsFolder" ) );
            Reference< XRow > xRow( getPropertyValues( aProps, NULL ), UNO_SET_THROW );
            bIsFolder = xRow->getBoolean(1);
        }
        catch( const Exception& )
        {
        	DBG_UNHANDLED_EXCEPTION();
        }
        m_aIsFolder.reset( bIsFolder );
        return *m_aIsFolder;
    }

    //------------------------------------------------------------------------------------------------------------------
    void Content::impl_determineContentType()
    {
        if ( !!m_aContentType )
            return;

        m_aContentType.reset( ContentProvider::getArtificialNodeContentType() );
        if ( m_eExtContentType == E_EXTENSION_CONTENT )
        {
            try
            {
                Sequence< Property > aProps(1);
                aProps[0].Name = ::rtl::OUString( RTL_CONSTASCII_USTRINGPARAM( "ContentType" ) );
                Reference< XRow > xRow( getPropertyValues( aProps, NULL ), UNO_SET_THROW );
                m_aContentType.reset( xRow->getString(1) );
            }
            catch( const Exception& )
            {
        	    DBG_UNHANDLED_EXCEPTION();
            }
        }
    }

//......................................................................................................................
} } }   // namespace ucp::ext
//......................................................................................................................
