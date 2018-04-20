//
//  DomainServer.cpp
//  domain-server/src
//
//  Created by Stephen Birarda on 9/26/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "DomainServer.h"

#include <memory>
#include <random>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QSharedMemory>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QCommandLineParser>
#include <QUuid>

#include <AccountManager.h>
#include <AssetClient.h>
#include <BuildInfo.h>
#include <DependencyManager.h>
#include <HifiConfigVariantMap.h>
#include <HTTPConnection.h>
#include <LogUtils.h>
#include <NetworkingConstants.h>
#include <udt/PacketHeaders.h>
#include <SettingHandle.h>
#include <SharedUtil.h>
#include <ShutdownEventListener.h>
#include <UUID.h>
#include <LogHandler.h>
#include <PathUtils.h>
#include <NumericalConstants.h>
#include <Trace.h>
#include <StatTracker.h>

#include "AssetsBackupHandler.h"
#include "ContentSettingsBackupHandler.h"
#include "DomainServerNodeData.h"
#include "EntitiesBackupHandler.h"
#include "NodeConnectionData.h"

#include <Gzip.h>

#include <OctreeDataUtils.h>

Q_LOGGING_CATEGORY(domain_server, "hifi.domain_server")

const QString ACCESS_TOKEN_KEY_PATH = "metaverse.access_token";
const QString DomainServer::REPLACEMENT_FILE_EXTENSION = ".replace";

int const DomainServer::EXIT_CODE_REBOOT = 234923;

#if USE_STABLE_GLOBAL_SERVICES
const QString ICE_SERVER_DEFAULT_HOSTNAME = "ice.highfidelity.com";
#else
const QString ICE_SERVER_DEFAULT_HOSTNAME = "dev-ice.highfidelity.com";
#endif

bool DomainServer::forwardMetaverseAPIRequest(HTTPConnection* connection,
                                              const QString& metaversePath,
                                              const QString& requestSubobjectKey,
                                              std::initializer_list<QString> requiredData,
                                              std::initializer_list<QString> optionalData,
                                              bool requireAccessToken) {

    auto accessTokenVariant = _settingsManager.valueForKeyPath(ACCESS_TOKEN_KEY_PATH);
    if (!accessTokenVariant.isValid() && requireAccessToken) {
        connection->respond(HTTPConnection::StatusCode400, "User access token has not been set");
        return true;
    }

    QJsonObject subobject;

    auto params = connection->parseUrlEncodedForm();

    for (auto& key : requiredData) {
        auto it = params.find(key);
        if (it == params.end()) {
            auto error = "Bad request, expected param '" + key + "'";
            connection->respond(HTTPConnection::StatusCode400, error.toLatin1());
            return true;
        }
        subobject.insert(key, it.value());
    }

    for (auto& key : optionalData) {
        auto it = params.find(key);
        if (it != params.end()) {
            subobject.insert(key, it.value());
        }
    }

    QJsonObject root;
    root.insert(requestSubobjectKey, subobject);
    QJsonDocument doc { root };

    QUrl url { NetworkingConstants::METAVERSE_SERVER_URL().toString() + metaversePath };

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (accessTokenVariant.isValid()) {
        auto accessTokenHeader = QString("Bearer ") + accessTokenVariant.toString();
        req.setRawHeader("Authorization", accessTokenHeader.toLatin1());
    }

    QNetworkReply* reply;
    auto method = connection->requestOperation();
    if (method == QNetworkAccessManager::GetOperation) {
        reply = NetworkAccessManager::getInstance().get(req);
    } else if (method == QNetworkAccessManager::PostOperation) {
        reply = NetworkAccessManager::getInstance().post(req, doc.toJson());
    } else if (method == QNetworkAccessManager::PutOperation) {
        reply = NetworkAccessManager::getInstance().put(req, doc.toJson());
    } else {
        connection->respond(HTTPConnection::StatusCode400, "Error forwarding request, unsupported method");
        return true;
    }

    connect(reply, &QNetworkReply::finished, this, [reply, connection]() {
        if (reply->error() != QNetworkReply::NoError) {
            auto data = reply->readAll();
            qDebug() << "Got error response from metaverse server (" << reply->url() << "): " << data << reply->errorString();
            connection->respond(HTTPConnection::StatusCode400, data);
            return;
        }

        connection->respond(HTTPConnection::StatusCode200, reply->readAll());
    });

    return true;
}

DomainServer::DomainServer(int argc, char* argv[]) :
    QCoreApplication(argc, argv),
    _gatekeeper(this),
    _httpManager(QHostAddress::AnyIPv4, DOMAIN_SERVER_HTTP_PORT, QString("%1/resources/web/").arg(QCoreApplication::applicationDirPath()), this),
    _httpsManager(NULL),
    _allAssignments(),
    _unfulfilledAssignments(),
    _isUsingDTLS(false),
    _oauthProviderURL(),
    _oauthClientID(),
    _hostname(),
    _ephemeralACScripts(),
    _webAuthenticationStateSet(),
    _cookieSessionHash(),
    _automaticNetworkingSetting(),
    _settingsManager(),
    _iceServerAddr(ICE_SERVER_DEFAULT_HOSTNAME),
    _iceServerPort(ICE_SERVER_DEFAULT_PORT)
{
    PathUtils::removeTemporaryApplicationDirs();

    parseCommandLine();

    DependencyManager::set<tracing::Tracer>();
    DependencyManager::set<StatTracker>();

    LogUtils::init();

    qDebug() << "Setting up domain-server";
    qDebug() << "[VERSION] Build sequence:" << qPrintable(applicationVersion());
    qDebug() << "[VERSION] MODIFIED_ORGANIZATION:" << BuildInfo::MODIFIED_ORGANIZATION;
    qDebug() << "[VERSION] VERSION:" << BuildInfo::VERSION;
    qDebug() << "[VERSION] BUILD_BRANCH:" << BuildInfo::BUILD_BRANCH;
    qDebug() << "[VERSION] BUILD_GLOBAL_SERVICES:" << BuildInfo::BUILD_GLOBAL_SERVICES;
    qDebug() << "[VERSION] We will be using this name to find ICE servers:" << _iceServerAddr;


    // make sure we have a fresh AccountManager instance
    // (need this since domain-server can restart itself and maintain static variables)
    DependencyManager::set<AccountManager>();

    auto args = arguments();

    _settingsManager.setupConfigMap(args);

    // setup a shutdown event listener to handle SIGTERM or WM_CLOSE for us
#ifdef _WIN32
    installNativeEventFilter(&ShutdownEventListener::getInstance());
#else
    ShutdownEventListener::getInstance();
#endif

    qRegisterMetaType<DomainServerWebSessionData>("DomainServerWebSessionData");
    qRegisterMetaTypeStreamOperators<DomainServerWebSessionData>("DomainServerWebSessionData");

    // make sure we hear about newly connected nodes from our gatekeeper
    connect(&_gatekeeper, &DomainGatekeeper::connectedNode, this, &DomainServer::handleConnectedNode);

    // if a connected node loses connection privileges, hang up on it
    connect(&_gatekeeper, &DomainGatekeeper::killNode, this, &DomainServer::handleKillNode);

    // if permissions are updated, relay the changes to the Node datastructures
    connect(&_settingsManager, &DomainServerSettingsManager::updateNodePermissions,
            &_gatekeeper, &DomainGatekeeper::updateNodePermissions);
    connect(&_settingsManager, &DomainServerSettingsManager::settingsUpdated,
            this, &DomainServer::updateReplicatedNodes);
    connect(&_settingsManager, &DomainServerSettingsManager::settingsUpdated,
            this, &DomainServer::updateDownstreamNodes);
    connect(&_settingsManager, &DomainServerSettingsManager::settingsUpdated,
            this, &DomainServer::updateUpstreamNodes);

    setupGroupCacheRefresh();

    // if we were given a certificate/private key or oauth credentials they must succeed
    if (!(optionallyReadX509KeyAndCertificate() && optionallySetupOAuth())) {
        return;
    }

    _settingsManager.apiRefreshGroupInformation();

    setupNodeListAndAssignments();

    updateReplicatedNodes();
    updateDownstreamNodes();
    updateUpstreamNodes();

    if (_type != NonMetaverse) {
        // if we have a metaverse domain, we'll use an access token for API calls
        resetAccountManagerAccessToken();

        setupAutomaticNetworking();
    }

    if (!getID().isNull() && _type != NonMetaverse) {
        // setup periodic heartbeats to metaverse API
        setupHeartbeatToMetaverse();

        // send the first heartbeat immediately
        sendHeartbeatToMetaverse();
    }

    // check for the temporary name parameter
    const QString GET_TEMPORARY_NAME_SWITCH = "--get-temp-name";
    if (args.contains(GET_TEMPORARY_NAME_SWITCH)) {
        getTemporaryName();
    }

    // send signal to DomainMetadata when descriptors changed
    _metadata = new DomainMetadata(this);
    connect(&_settingsManager, &DomainServerSettingsManager::settingsUpdated,
            _metadata, &DomainMetadata::descriptorsChanged);

    qDebug() << "domain-server is running";
    static const QString AC_SUBNET_WHITELIST_SETTING_PATH = "security.ac_subnet_whitelist";

    static const Subnet LOCALHOST { QHostAddress("127.0.0.1"), 32 };
    _acSubnetWhitelist = { LOCALHOST };

    auto whitelist = _settingsManager.valueOrDefaultValueForKeyPath(AC_SUBNET_WHITELIST_SETTING_PATH).toStringList();
    for (auto& subnet : whitelist) {
        auto netmaskParts = subnet.trimmed().split("/");

        if (netmaskParts.size() > 2) {
            qDebug() << "Ignoring subnet in whitelist, malformed: " << subnet;
            continue;
        }

        // The default netmask is 32 if one has not been specified, which will
        // match only the ip provided.
        int netmask = 32;

        if (netmaskParts.size() == 2) {
            bool ok;
            netmask = netmaskParts[1].toInt(&ok);
            if (!ok) {
                qDebug() << "Ignoring subnet in whitelist, bad netmask: " << subnet;
                continue;
            }
        }

        auto ip = QHostAddress(netmaskParts[0]);

        if (!ip.isNull()) {
            qDebug() << "Adding AC whitelist subnet: " << subnet << " -> " << (ip.toString() + "/" + QString::number(netmask));
            _acSubnetWhitelist.push_back({ ip , netmask });
        } else {
            qDebug() << "Ignoring subnet in whitelist, invalid ip portion: " << subnet;
        }
    }

    if (QDir(getEntitiesDirPath()).mkpath(".")) {
        qCDebug(domain_server) << "Created entities data directory";
    }
    maybeHandleReplacementEntityFile();


    static const QString BACKUP_RULES_KEYPATH = AUTOMATIC_CONTENT_ARCHIVES_GROUP + ".backup_rules";
    auto backupRulesVariant = _settingsManager.valueOrDefaultValueForKeyPath(BACKUP_RULES_KEYPATH);

    _contentManager.reset(new DomainContentBackupManager(getContentBackupDir(), backupRulesVariant.toList()));

    connect(_contentManager.get(), &DomainContentBackupManager::started, _contentManager.get(), [this](){
        _contentManager->addBackupHandler(BackupHandlerPointer(new EntitiesBackupHandler(getEntitiesFilePath(), getEntitiesReplacementFilePath())));
        _contentManager->addBackupHandler(BackupHandlerPointer(new AssetsBackupHandler(getContentBackupDir())));
        _contentManager->addBackupHandler(BackupHandlerPointer(new ContentSettingsBackupHandler(_settingsManager)));
    });

    _contentManager->initialize(true);

    connect(_contentManager.get(), &DomainContentBackupManager::recoveryCompleted, this, &DomainServer::restart);
}

void DomainServer::parseCommandLine() {
    QCommandLineParser parser;
    parser.setApplicationDescription("High Fidelity Domain Server");
    parser.addHelpOption();

    const QCommandLineOption iceServerAddressOption("i", "ice-server address", "IP:PORT or HOSTNAME:PORT");
    parser.addOption(iceServerAddressOption);

    const QCommandLineOption domainIDOption("d", "domain-server uuid");
    parser.addOption(domainIDOption);

    const QCommandLineOption getTempNameOption("get-temp-name", "Request a temporary domain-name");
    parser.addOption(getTempNameOption);

    const QCommandLineOption masterConfigOption("master-config", "Deprecated config-file option");
    parser.addOption(masterConfigOption);

    const QCommandLineOption parentPIDOption(PARENT_PID_OPTION, "PID of the parent process", "parent-pid");
    parser.addOption(parentPIDOption);

    if (!parser.parse(QCoreApplication::arguments())) {
        qWarning() << parser.errorText() << endl;
        parser.showHelp();
        Q_UNREACHABLE();
    }

    if (parser.isSet(iceServerAddressOption)) {
        // parse the IP and port combination for this target
        QString hostnamePortString = parser.value(iceServerAddressOption);

        _iceServerAddr = hostnamePortString.left(hostnamePortString.indexOf(':'));
        _iceServerPort = (quint16) hostnamePortString.mid(hostnamePortString.indexOf(':') + 1).toUInt();
        if (_iceServerPort == 0) {
            _iceServerPort = ICE_SERVER_DEFAULT_PORT;
        }

        if (_iceServerAddr.isEmpty()) {
            qWarning() << "Could not parse an IP address and port combination from" << hostnamePortString;
            QMetaObject::invokeMethod(this, "quit", Qt::QueuedConnection);
        }
    }

    if (parser.isSet(domainIDOption)) {
        _overridingDomainID = QUuid(parser.value(domainIDOption));
        _overrideDomainID = true;
        qDebug() << "domain-server ID is" << _overridingDomainID;
    }


    if (parser.isSet(parentPIDOption)) {
        bool ok = false;
        int parentPID = parser.value(parentPIDOption).toInt(&ok);

        if (ok) {
            qDebug() << "Parent process PID is" << parentPID;
            watchParentProcess(parentPID);
        }
    }
}

DomainServer::~DomainServer() {
    qInfo() << "Domain Server is shutting down.";

    if (_contentManager) {
        _contentManager->aboutToFinish();
        _contentManager->terminate();
    }

    // cleanup the AssetClient thread
    DependencyManager::destroy<AssetClient>();
    _assetClientThread.quit();
    _assetClientThread.wait();

    // destroy the LimitedNodeList before the DomainServer QCoreApplication is down
    DependencyManager::destroy<LimitedNodeList>();
}

void DomainServer::queuedQuit(QString quitMessage, int exitCode) {
    if (!quitMessage.isEmpty()) {
        qWarning() << qPrintable(quitMessage);
    }

    QCoreApplication::exit(exitCode);
}

void DomainServer::restart() {
    qDebug() << "domain-server is restarting.";

    exit(DomainServer::EXIT_CODE_REBOOT);
}

QUuid DomainServer::getID() {
    return DependencyManager::get<LimitedNodeList>()->getSessionUUID();
}

bool DomainServer::optionallyReadX509KeyAndCertificate() {
    const QString X509_CERTIFICATE_OPTION = "cert";
    const QString X509_PRIVATE_KEY_OPTION = "key";
    const QString X509_KEY_PASSPHRASE_ENV = "DOMAIN_SERVER_KEY_PASSPHRASE";

    QString certPath = _settingsManager.valueForKeyPath(X509_CERTIFICATE_OPTION).toString();
    QString keyPath = _settingsManager.valueForKeyPath(X509_PRIVATE_KEY_OPTION).toString();

    if (!certPath.isEmpty() && !keyPath.isEmpty()) {
        // the user wants to use the following cert and key for HTTPS
        // this is used for Oauth callbacks when authorizing users against a data server
        // let's make sure we can load the key and certificate

        QString keyPassphraseString = QProcessEnvironment::systemEnvironment().value(X509_KEY_PASSPHRASE_ENV);

        qDebug() << "Reading certificate file at" << certPath << "for HTTPS.";
        qDebug() << "Reading key file at" << keyPath << "for HTTPS.";

        QFile certFile(certPath);
        certFile.open(QIODevice::ReadOnly);

        QFile keyFile(keyPath);
        keyFile.open(QIODevice::ReadOnly);

        QSslCertificate sslCertificate(&certFile);
        QSslKey privateKey(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, keyPassphraseString.toUtf8());

        _httpsManager = new HTTPSManager(QHostAddress::AnyIPv4, DOMAIN_SERVER_HTTPS_PORT, sslCertificate, privateKey, QString(), this, this);

        qDebug() << "TCP server listening for HTTPS connections on" << DOMAIN_SERVER_HTTPS_PORT;

    } else if (!certPath.isEmpty() || !keyPath.isEmpty()) {
        static const QString MISSING_CERT_ERROR_MSG = "Missing certificate or private key. domain-server will now quit.";
        static const int MISSING_CERT_ERROR_CODE = 3;

        QMetaObject::invokeMethod(this, "queuedQuit", Qt::QueuedConnection,
                                  Q_ARG(QString, MISSING_CERT_ERROR_MSG), Q_ARG(int, MISSING_CERT_ERROR_CODE));
        return false;
    }

    return true;
}

bool DomainServer::optionallySetupOAuth() {
    const QString OAUTH_PROVIDER_URL_OPTION = "oauth-provider";
    const QString OAUTH_CLIENT_ID_OPTION = "oauth-client-id";
    const QString OAUTH_CLIENT_SECRET_ENV = "DOMAIN_SERVER_CLIENT_SECRET";
    const QString REDIRECT_HOSTNAME_OPTION = "hostname";

    _oauthProviderURL = QUrl(_settingsManager.valueForKeyPath(OAUTH_PROVIDER_URL_OPTION).toString());

    // if we don't have an oauth provider URL then we default to the default node auth url
    if (_oauthProviderURL.isEmpty()) {
        _oauthProviderURL = NetworkingConstants::METAVERSE_SERVER_URL();
    }

    auto accountManager = DependencyManager::get<AccountManager>();
    accountManager->setAuthURL(_oauthProviderURL);

    _oauthClientID = _settingsManager.valueForKeyPath(OAUTH_CLIENT_ID_OPTION).toString();
    _oauthClientSecret = QProcessEnvironment::systemEnvironment().value(OAUTH_CLIENT_SECRET_ENV);
    _hostname = _settingsManager.valueForKeyPath(REDIRECT_HOSTNAME_OPTION).toString();

    if (!_oauthClientID.isEmpty()) {
        if (_oauthProviderURL.isEmpty()
            || _hostname.isEmpty()
            || _oauthClientID.isEmpty()
            || _oauthClientSecret.isEmpty()) {
            static const QString MISSING_OAUTH_INFO_MSG = "Missing OAuth provider URL, hostname, client ID, or client secret. domain-server will now quit.";
            static const int MISSING_OAUTH_INFO_ERROR_CODE = 4;
            QMetaObject::invokeMethod(this, "queuedQuit", Qt::QueuedConnection,
                                      Q_ARG(QString, MISSING_OAUTH_INFO_MSG), Q_ARG(int, MISSING_OAUTH_INFO_ERROR_CODE));
            return false;
        } else {
            qDebug() << "OAuth will be used to identify clients using provider at" << _oauthProviderURL.toString();
            qDebug() << "OAuth Client ID is" << _oauthClientID;
        }
    }

    return true;
}

static const QString METAVERSE_DOMAIN_ID_KEY_PATH = "metaverse.id";

void DomainServer::getTemporaryName(bool force) {
    // check if we already have a domain ID
    QVariant idValueVariant = _settingsManager.valueForKeyPath(METAVERSE_DOMAIN_ID_KEY_PATH);

    qInfo() << "Requesting temporary domain name";
    if (idValueVariant.isValid()) {
        qDebug() << "A domain ID is already present in domain-server settings:" << idValueVariant.toString();
        if (force) {
            qDebug() << "Requesting temporary domain name to replace current ID:" << getID();
        } else {
            qInfo() << "Abandoning request of temporary domain name.";
            return;
        }
    }

    // request a temporary name from the metaverse
    auto accountManager = DependencyManager::get<AccountManager>();
    JSONCallbackParameters callbackParameters { this, "handleTempDomainSuccess", this, "handleTempDomainError" };
    accountManager->sendRequest("/api/v1/domains/temporary", AccountManagerAuth::None,
                                QNetworkAccessManager::PostOperation, callbackParameters);
}

void DomainServer::handleTempDomainSuccess(QNetworkReply& requestReply) {
    QJsonObject jsonObject = QJsonDocument::fromJson(requestReply.readAll()).object();

    // grab the information for the new domain
    static const QString DATA_KEY = "data";
    static const QString DOMAIN_KEY = "domain";
    static const QString ID_KEY = "id";
    static const QString NAME_KEY = "name";
    static const QString KEY_KEY = "api_key";

    auto domainObject = jsonObject[DATA_KEY].toObject()[DOMAIN_KEY].toObject();
    if (!domainObject.isEmpty()) {
        auto id = _overrideDomainID ? _overridingDomainID.toString() : domainObject[ID_KEY].toString();
        auto name = domainObject[NAME_KEY].toString();
        auto key = domainObject[KEY_KEY].toString();

        qInfo() << "Received new temporary domain name" << name;
        qDebug() << "The temporary domain ID is" << id;

        // store the new domain ID and auto network setting immediately
        QString newSettingsJSON = QString("{\"metaverse\": { \"id\": \"%1\", \"automatic_networking\": \"full\"}}").arg(id);
        auto settingsDocument = QJsonDocument::fromJson(newSettingsJSON.toUtf8());
        _settingsManager.recurseJSONObjectAndOverwriteSettings(settingsDocument.object(), DomainSettings);

        // store the new token to the account info
        auto accountManager = DependencyManager::get<AccountManager>();
        accountManager->setTemporaryDomain(id, key);

        // change our domain ID immediately
        DependencyManager::get<LimitedNodeList>()->setSessionUUID(QUuid { id });

        // change our type to reflect that we are a temporary domain now
        _type = MetaverseTemporaryDomain;

        // update our heartbeats to use the correct id
        setupICEHeartbeatForFullNetworking();
        setupHeartbeatToMetaverse();

        // if we have a current ICE server address, update it in the API for the new temporary domain
        sendICEServerAddressToMetaverseAPI();
    } else {
        qWarning() << "There were problems parsing the API response containing a temporary domain name. Please try again"
            << "via domain-server relaunch or from the domain-server settings.";
    }
}

void DomainServer::handleTempDomainError(QNetworkReply& requestReply) {
    qWarning() << "A temporary name was requested but there was an error creating one. Please try again via domain-server relaunch"
        << "or from the domain-server settings.";
}

const QString DOMAIN_CONFIG_ID_KEY = "id";

const QString METAVERSE_AUTOMATIC_NETWORKING_KEY_PATH = "metaverse.automatic_networking";
const QString FULL_AUTOMATIC_NETWORKING_VALUE = "full";
const QString IP_ONLY_AUTOMATIC_NETWORKING_VALUE = "ip";
const QString DISABLED_AUTOMATIC_NETWORKING_VALUE = "disabled";



bool DomainServer::isPacketVerified(const udt::Packet& packet) {
    PacketType headerType = NLPacket::typeInHeader(packet);
    PacketVersion headerVersion = NLPacket::versionInHeader(packet);

    auto nodeList = DependencyManager::get<LimitedNodeList>();

    // if this is a mismatching connect packet, we can't simply drop it on the floor
    // send back a packet to the interface that tells them we refuse connection for a mismatch
    if (headerType == PacketType::DomainConnectRequest
        && headerVersion != versionForPacketType(PacketType::DomainConnectRequest)) {
        DomainGatekeeper::sendProtocolMismatchConnectionDenial(packet.getSenderSockAddr());
    }

    if (!PacketTypeEnum::getNonSourcedPackets().contains(headerType)) {
        // this is a sourced packet - first check if we have a node that matches
        Node::LocalID localSourceID = NLPacket::sourceIDInHeader(packet);
        SharedNodePointer sourceNode = nodeList->nodeWithLocalID(localSourceID);

        if (sourceNode) {
            // unverified DS packets (due to a lack of connection secret between DS + node)
            // must come either from the same public IP address or a local IP address (set by RFC 1918)

            DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(sourceNode->getLinkedData());

            bool exactAddressMatch = nodeData->getSendingSockAddr() == packet.getSenderSockAddr();
            bool bothPrivateAddresses = nodeData->getSendingSockAddr().hasPrivateAddress()
                && packet.getSenderSockAddr().hasPrivateAddress();

            if (nodeData && (exactAddressMatch || bothPrivateAddresses)) {
                // to the best of our ability we've verified that this packet comes from the right place
                // let the NodeList do its checks now (but pass it the sourceNode so it doesn't need to look it up again)
                return nodeList->isPacketVerifiedWithSource(packet, sourceNode.data());
            } else {
                HIFI_FDEBUG("Packet of type" << headerType
                    << "received from unmatched IP for UUID" << uuidStringWithoutCurlyBraces(sourceNode->getUUID()));

                return false;
            }
        } else {
            HIFI_FDEBUG("Packet of type" << headerType
                << "received from unknown node with UUID" << uuidStringWithoutCurlyBraces(sourceNode->getUUID()));

            return false;
        }
    }

    // fallback to allow the normal NodeList implementation to verify packets
    return nodeList->isPacketVerified(packet);
}


void DomainServer::setupNodeListAndAssignments() {
    const QString CUSTOM_LOCAL_PORT_OPTION = "metaverse.local_port";

    QVariant localPortValue = _settingsManager.valueOrDefaultValueForKeyPath(CUSTOM_LOCAL_PORT_OPTION);
    int domainServerPort = localPortValue.toInt();

    int domainServerDTLSPort = INVALID_PORT;

    if (_isUsingDTLS) {
        domainServerDTLSPort = DEFAULT_DOMAIN_SERVER_DTLS_PORT;

        const QString CUSTOM_DTLS_PORT_OPTION = "dtls-port";

        auto dtlsPortVariant = _settingsManager.valueForKeyPath(CUSTOM_DTLS_PORT_OPTION);
        if (dtlsPortVariant.isValid()) {
            domainServerDTLSPort = (unsigned short) dtlsPortVariant.toUInt();
        }
    }

    QSet<Assignment::Type> parsedTypes;
    parseAssignmentConfigs(parsedTypes);

    populateDefaultStaticAssignmentsExcludingTypes(parsedTypes);

    // check for scripts the user wants to persist from their domain-server config
    populateStaticScriptedAssignmentsFromSettings();

    auto nodeList = DependencyManager::set<LimitedNodeList>(domainServerPort, domainServerDTLSPort);

    // no matter the local port, save it to shared mem so that local assignment clients can ask what it is
    nodeList->putLocalPortIntoSharedMemory(DOMAIN_SERVER_LOCAL_PORT_SMEM_KEY, this, nodeList->getSocketLocalPort());

    // store our local http ports in shared memory
    quint16 localHttpPort = DOMAIN_SERVER_HTTP_PORT;
    nodeList->putLocalPortIntoSharedMemory(DOMAIN_SERVER_LOCAL_HTTP_PORT_SMEM_KEY, this, localHttpPort);
    quint16 localHttpsPort = DOMAIN_SERVER_HTTPS_PORT;
    nodeList->putLocalPortIntoSharedMemory(DOMAIN_SERVER_LOCAL_HTTPS_PORT_SMEM_KEY, this, localHttpsPort);

    // set our LimitedNodeList UUID to match the UUID from our config
    // nodes will currently use this to add resources to data-web that relate to our domain
    bool isMetaverseDomain = false;
    if (_overrideDomainID) {
        nodeList->setSessionUUID(_overridingDomainID);
        isMetaverseDomain = true; // assume metaverse domain
    } else {
        QVariant idValueVariant = _settingsManager.valueForKeyPath(METAVERSE_DOMAIN_ID_KEY_PATH);
        if (idValueVariant.isValid()) {
            nodeList->setSessionUUID(idValueVariant.toString());
            isMetaverseDomain = true; // if we have an ID, we'll assume we're a metaverse domain
        } else {
            nodeList->setSessionUUID(QUuid::createUuid()); // Use random UUID
        }
    }

    // Create our own short session ID.
    Node::LocalID serverSessionLocalID = _gatekeeper.findOrCreateLocalID(nodeList->getSessionUUID());
    nodeList->setSessionLocalID(serverSessionLocalID);

    if (isMetaverseDomain) {
        // see if we think we're a temp domain (we have an API key) or a full domain
        const auto& temporaryDomainKey = DependencyManager::get<AccountManager>()->getTemporaryDomainKey(getID());
        if (temporaryDomainKey.isEmpty()) {
            _type = MetaverseDomain;
        } else {
            _type = MetaverseTemporaryDomain;
        }
    }

    connect(nodeList.data(), &LimitedNodeList::nodeAdded, this, &DomainServer::nodeAdded);
    connect(nodeList.data(), &LimitedNodeList::nodeKilled, this, &DomainServer::nodeKilled);

    // register as the packet receiver for the types we want
    PacketReceiver& packetReceiver = nodeList->getPacketReceiver();
    packetReceiver.registerListener(PacketType::RequestAssignment, this, "processRequestAssignmentPacket");
    packetReceiver.registerListener(PacketType::DomainListRequest, this, "processListRequestPacket");
    packetReceiver.registerListener(PacketType::DomainServerPathQuery, this, "processPathQueryPacket");
    packetReceiver.registerListener(PacketType::NodeJsonStats, this, "processNodeJSONStatsPacket");
    packetReceiver.registerListener(PacketType::DomainDisconnectRequest, this, "processNodeDisconnectRequestPacket");

    // NodeList won't be available to the settings manager when it is created, so call registerListener here
    packetReceiver.registerListener(PacketType::DomainSettingsRequest, &_settingsManager, "processSettingsRequestPacket");
    packetReceiver.registerListener(PacketType::NodeKickRequest, &_settingsManager, "processNodeKickRequestPacket");
    packetReceiver.registerListener(PacketType::UsernameFromIDRequest, &_settingsManager, "processUsernameFromIDRequestPacket");

    // register the gatekeeper for the packets it needs to receive
    packetReceiver.registerListener(PacketType::DomainConnectRequest, &_gatekeeper, "processConnectRequestPacket");
    packetReceiver.registerListener(PacketType::ICEPing, &_gatekeeper, "processICEPingPacket");
    packetReceiver.registerListener(PacketType::ICEPingReply, &_gatekeeper, "processICEPingReplyPacket");
    packetReceiver.registerListener(PacketType::ICEServerPeerInformation, &_gatekeeper, "processICEPeerInformationPacket");

    packetReceiver.registerListener(PacketType::ICEServerHeartbeatDenied, this, "processICEServerHeartbeatDenialPacket");
    packetReceiver.registerListener(PacketType::ICEServerHeartbeatACK, this, "processICEServerHeartbeatACK");

    packetReceiver.registerListener(PacketType::OctreeDataFileRequest, this, "processOctreeDataRequestMessage");
    packetReceiver.registerListener(PacketType::OctreeDataPersist, this, "processOctreeDataPersistMessage");

    packetReceiver.registerListener(PacketType::OctreeFileReplacement, this, "handleOctreeFileReplacementRequest");
    packetReceiver.registerListener(PacketType::DomainContentReplacementFromUrl, this, "handleDomainContentReplacementFromURLRequest");

    // set a custom packetVersionMatch as the verify packet operator for the udt::Socket
    nodeList->setPacketFilterOperator(&DomainServer::isPacketVerified);

    _assetClientThread.setObjectName("AssetClient Thread");
    auto assetClient = DependencyManager::set<AssetClient>();
    assetClient->moveToThread(&_assetClientThread);
    _assetClientThread.start();
    // add whatever static assignments that have been parsed to the queue
    addStaticAssignmentsToQueue();
}

bool DomainServer::resetAccountManagerAccessToken() {
    if (!_oauthProviderURL.isEmpty()) {
        // check for an access-token in our settings, can optionally be overidden by env value
        const QString ENV_ACCESS_TOKEN_KEY = "DOMAIN_SERVER_ACCESS_TOKEN";

        QString accessToken = QProcessEnvironment::systemEnvironment().value(ENV_ACCESS_TOKEN_KEY);

        if (accessToken.isEmpty()) {
            QVariant accessTokenVariant = _settingsManager.valueForKeyPath(ACCESS_TOKEN_KEY_PATH);

            if (accessTokenVariant.canConvert(QMetaType::QString)) {
                accessToken = accessTokenVariant.toString();
            } else {
                qWarning() << "No access token is present. Some operations that use the metaverse API will fail.";
                qDebug() << "Set an access token via the web interface, in your user config"
                    << "at keypath metaverse.access_token or in your ENV at key DOMAIN_SERVER_ACCESS_TOKEN";

                // clear any existing access token from AccountManager
                DependencyManager::get<AccountManager>()->setAccessTokenForCurrentAuthURL(QString());

                return false;
            }
        } else {
            qDebug() << "Using access token from DOMAIN_SERVER_ACCESS_TOKEN in env. This overrides any access token present"
                << " in the user config.";
        }

        // give this access token to the AccountManager
        DependencyManager::get<AccountManager>()->setAccessTokenForCurrentAuthURL(accessToken);

        return true;

    } else {
        static const QString MISSING_OAUTH_PROVIDER_MSG =
            QString("Missing OAuth provider URL, but a domain-server feature was required that requires authentication.") +
            QString("domain-server will now quit.");
        static const int MISSING_OAUTH_PROVIDER_ERROR_CODE = 5;
        QMetaObject::invokeMethod(this, "queuedQuit", Qt::QueuedConnection,
                                  Q_ARG(QString, MISSING_OAUTH_PROVIDER_MSG),
                                  Q_ARG(int, MISSING_OAUTH_PROVIDER_ERROR_CODE));

        return false;
    }
}

void DomainServer::setupAutomaticNetworking() {

    _automaticNetworkingSetting =
        _settingsManager.valueOrDefaultValueForKeyPath(METAVERSE_AUTOMATIC_NETWORKING_KEY_PATH).toString();

    qDebug() << "Configuring automatic networking in domain-server as" << _automaticNetworkingSetting;

    if (_automaticNetworkingSetting != DISABLED_AUTOMATIC_NETWORKING_VALUE) {
        const QUuid& domainID = getID();

        if (_automaticNetworkingSetting == FULL_AUTOMATIC_NETWORKING_VALUE) {
            setupICEHeartbeatForFullNetworking();
        }

        if (_automaticNetworkingSetting == IP_ONLY_AUTOMATIC_NETWORKING_VALUE ||
            _automaticNetworkingSetting == FULL_AUTOMATIC_NETWORKING_VALUE) {

            if (!domainID.isNull()) {
                qDebug() << "domain-server" << _automaticNetworkingSetting << "automatic networking enabled for ID"
                    << uuidStringWithoutCurlyBraces(domainID) << "via" << _oauthProviderURL.toString();

                if (_automaticNetworkingSetting == IP_ONLY_AUTOMATIC_NETWORKING_VALUE) {

                    auto nodeList = DependencyManager::get<LimitedNodeList>();

                    // send any public socket changes to the data server so nodes can find us at our new IP
                    connect(nodeList.data(), &LimitedNodeList::publicSockAddrChanged,
                            this, &DomainServer::performIPAddressUpdate);

                    // have the LNL enable public socket updating via STUN
                    nodeList->startSTUNPublicSocketUpdate();
                }
            } else {
                qDebug() << "Cannot enable domain-server automatic networking without a domain ID."
                << "Please add an ID to your config file or via the web interface.";
                return;
            }
        }
    }

}

void DomainServer::setupHeartbeatToMetaverse() {
    // heartbeat to the data-server every 15s
    const int DOMAIN_SERVER_DATA_WEB_HEARTBEAT_MSECS = 15 * 1000;

    if (!_metaverseHeartbeatTimer) {
        // setup a timer to heartbeat with the metaverse-server
        _metaverseHeartbeatTimer = new QTimer { this };
        connect(_metaverseHeartbeatTimer, SIGNAL(timeout()), this, SLOT(sendHeartbeatToMetaverse()));
        // do not send a heartbeat immediately - this avoids flooding if the heartbeat fails with a 401
        _metaverseHeartbeatTimer->start(DOMAIN_SERVER_DATA_WEB_HEARTBEAT_MSECS);
    }
}

void DomainServer::setupICEHeartbeatForFullNetworking() {
    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

    // lookup the available ice-server hosts now
    updateICEServerAddresses();

    // call our sendHeartbeatToIceServer immediately anytime a local or public socket changes
    connect(limitedNodeList.data(), &LimitedNodeList::localSockAddrChanged,
            this, &DomainServer::sendHeartbeatToIceServer);
    connect(limitedNodeList.data(), &LimitedNodeList::publicSockAddrChanged,
            this, &DomainServer::sendHeartbeatToIceServer);

    // we need this DS to know what our public IP is - start trying to figure that out now
    limitedNodeList->startSTUNPublicSocketUpdate();

    // to send ICE heartbeats we'd better have a private key locally with an uploaded public key
    // if we have an access token and we don't have a private key or the current domain ID has changed
    // we should generate a new keypair
    auto accountManager = DependencyManager::get<AccountManager>();
    if (!accountManager->getAccountInfo().hasPrivateKey() || accountManager->getAccountInfo().getDomainID() != getID()) {
        accountManager->generateNewDomainKeypair(getID());
    }

    // hookup to the signal from account manager that tells us when keypair is available
    connect(accountManager.data(), &AccountManager::newKeypair, this, &DomainServer::handleKeypairChange);

    if (!_iceHeartbeatTimer) {
        // setup a timer to heartbeat with the ice-server
        _iceHeartbeatTimer = new QTimer { this };
        connect(_iceHeartbeatTimer, &QTimer::timeout, this, &DomainServer::sendHeartbeatToIceServer);
        sendHeartbeatToIceServer();
        _iceHeartbeatTimer->start(ICE_HEARBEAT_INTERVAL_MSECS);
    }
}

void DomainServer::updateICEServerAddresses() {
    if (_iceAddressLookupID == INVALID_ICE_LOOKUP_ID) {
        _iceAddressLookupID = QHostInfo::lookupHost(_iceServerAddr, this, SLOT(handleICEHostInfo(QHostInfo)));
    }
}

void DomainServer::parseAssignmentConfigs(QSet<Assignment::Type>& excludedTypes) {
    const QString ASSIGNMENT_CONFIG_PREFIX = "config-";

    // scan for assignment config keys
    for (int i = 0; i < Assignment::AllTypes; ++i) {
        QVariant assignmentConfigVariant = _settingsManager.valueOrDefaultValueForKeyPath(ASSIGNMENT_CONFIG_PREFIX + QString::number(i));

        if (assignmentConfigVariant.isValid()) {
            // figure out which assignment type this matches
            Assignment::Type assignmentType = static_cast<Assignment::Type>(i);

            if (!excludedTypes.contains(assignmentType)) {
                QVariantList assignmentList = assignmentConfigVariant.toList();

                if (assignmentType != Assignment::AgentType) {
                    createStaticAssignmentsForType(assignmentType, assignmentList);
                }

                excludedTypes.insert(assignmentType);
            }
        }
    }
}

void DomainServer::addStaticAssignmentToAssignmentHash(Assignment* newAssignment) {
    qDebug() << "Inserting assignment" << *newAssignment << "to static assignment hash.";
    newAssignment->setIsStatic(true);
    _allAssignments.insert(newAssignment->getUUID(), SharedAssignmentPointer(newAssignment));
}

void DomainServer::populateStaticScriptedAssignmentsFromSettings() {
    const QString PERSISTENT_SCRIPTS_KEY_PATH = "scripts.persistent_scripts";
    QVariant persistentScriptsVariant = _settingsManager.valueOrDefaultValueForKeyPath(PERSISTENT_SCRIPTS_KEY_PATH);

    if (persistentScriptsVariant.isValid()) {
        QVariantList persistentScriptsList = persistentScriptsVariant.toList();
        foreach(const QVariant& persistentScriptVariant, persistentScriptsList) {
            QVariantMap persistentScript = persistentScriptVariant.toMap();

            const QString PERSISTENT_SCRIPT_URL_KEY = "url";
            const QString PERSISTENT_SCRIPT_NUM_INSTANCES_KEY = "num_instances";
            const QString PERSISTENT_SCRIPT_POOL_KEY = "pool";

            if (persistentScript.contains(PERSISTENT_SCRIPT_URL_KEY)) {
                // check how many instances of this script to add

                int numInstances = persistentScript[PERSISTENT_SCRIPT_NUM_INSTANCES_KEY].toInt();
                QString scriptURL = persistentScript[PERSISTENT_SCRIPT_URL_KEY].toString();

                QString scriptPool = persistentScript.value(PERSISTENT_SCRIPT_POOL_KEY).toString();

                qDebug() << "Adding" << numInstances << "of persistent script at URL" << scriptURL << "- pool" << scriptPool;

                for (int i = 0; i < numInstances; ++i) {
                    // add a scripted assignment to the queue for this instance
                    Assignment* scriptAssignment = new Assignment(Assignment::CreateCommand,
                                                                  Assignment::AgentType,
                                                                  scriptPool);
                    scriptAssignment->setPayload(scriptURL.toUtf8());

                    // add it to static hash so we know we have to keep giving it back out
                    addStaticAssignmentToAssignmentHash(scriptAssignment);
                }
            }
        }
    }
}

void DomainServer::createStaticAssignmentsForType(Assignment::Type type, const QVariantList &configList) {
    // we have a string for config for this type
    qDebug() << "Parsing config for assignment type" << type;

    int configCounter = 0;

    foreach(const QVariant& configVariant, configList) {
        if (configVariant.canConvert(QMetaType::QVariantMap)) {
            QVariantMap configMap = configVariant.toMap();

            // check the config string for a pool
            const QString ASSIGNMENT_POOL_KEY = "pool";

            QString assignmentPool = configMap.value(ASSIGNMENT_POOL_KEY).toString();
            if (!assignmentPool.isEmpty()) {
                configMap.remove(ASSIGNMENT_POOL_KEY);
            }

            ++configCounter;
            qDebug() << "Type" << type << "config" << configCounter << "=" << configMap;

            Assignment* configAssignment = new Assignment(Assignment::CreateCommand, type, assignmentPool);

            // setup the payload as a semi-colon separated list of key = value
            QStringList payloadStringList;
            foreach(const QString& payloadKey, configMap.keys()) {
                QString dashes = payloadKey.size() == 1 ? "-" : "--";
                payloadStringList << QString("%1%2 %3").arg(dashes).arg(payloadKey).arg(configMap[payloadKey].toString());
            }

            configAssignment->setPayload(payloadStringList.join(' ').toUtf8());

            addStaticAssignmentToAssignmentHash(configAssignment);
        }
    }
}

void DomainServer::populateDefaultStaticAssignmentsExcludingTypes(const QSet<Assignment::Type>& excludedTypes) {
    // enumerate over all assignment types and see if we've already excluded it
    for (Assignment::Type defaultedType = Assignment::FirstType;
         defaultedType != Assignment::AllTypes;
         defaultedType =  static_cast<Assignment::Type>(static_cast<int>(defaultedType) + 1)) {
        if (!excludedTypes.contains(defaultedType) && defaultedType != Assignment::AgentType) {

            if (defaultedType == Assignment::AssetServerType) {
                // Make sure the asset-server is enabled before adding it here.
                // Initially we do not assign it by default so we can test it in HF domains first
                static const QString ASSET_SERVER_ENABLED_KEYPATH = "asset_server.enabled";

                if (!_settingsManager.valueOrDefaultValueForKeyPath(ASSET_SERVER_ENABLED_KEYPATH).toBool()) {
                    // skip to the next iteration if asset-server isn't enabled
                    continue;
                }
            }

            // type has not been set from a command line or config file config, use the default
            // by clearing whatever exists and writing a single default assignment with no payload
            Assignment* newAssignment = new Assignment(Assignment::CreateCommand, (Assignment::Type) defaultedType);
            addStaticAssignmentToAssignmentHash(newAssignment);
        }
    }
}

void DomainServer::processListRequestPacket(QSharedPointer<ReceivedMessage> message, SharedNodePointer sendingNode) {
    QDataStream packetStream(message->getMessage());
    NodeConnectionData nodeRequestData = NodeConnectionData::fromDataStream(packetStream, message->getSenderSockAddr(), false);

    // update this node's sockets in case they have changed
    sendingNode->setPublicSocket(nodeRequestData.publicSockAddr);
    sendingNode->setLocalSocket(nodeRequestData.localSockAddr);

    // update the NodeInterestSet in case there have been any changes
    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(sendingNode->getLinkedData());

    // guard against patched agents asking to hear about other agents
    auto safeInterestSet = nodeRequestData.interestList.toSet();
    if (sendingNode->getType() == NodeType::Agent) {
        safeInterestSet.remove(NodeType::Agent);
    }

    nodeData->setNodeInterestSet(safeInterestSet);

    // update the connecting hostname in case it has changed
    nodeData->setPlaceName(nodeRequestData.placeName);

    sendDomainListToNode(sendingNode, message->getSenderSockAddr());
}

bool DomainServer::isInInterestSet(const SharedNodePointer& nodeA, const SharedNodePointer& nodeB) {
    auto nodeAData = static_cast<DomainServerNodeData*>(nodeA->getLinkedData());
    return nodeAData && nodeAData->getNodeInterestSet().contains(nodeB->getType());
}

unsigned int DomainServer::countConnectedUsers() {
    unsigned int result = 0;
    auto nodeList = DependencyManager::get<LimitedNodeList>();
    nodeList->eachNode([&](const SharedNodePointer& node){
        // only count unassigned agents (i.e., users)
        if (node->getType() == NodeType::Agent) {
            auto nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());
            if (nodeData && !nodeData->wasAssigned()) {
                result++;
            }
        }
    });
    return result;
}

QUrl DomainServer::oauthRedirectURL() {
    if (_httpsManager) {
        return QString("https://%1:%2/oauth").arg(_hostname).arg(_httpsManager->serverPort());
    } else {
        qWarning() << "Attempting to determine OAuth re-direct URL with no HTTPS server configured.";
        return QUrl();
    }
}

const QString OAUTH_CLIENT_ID_QUERY_KEY = "client_id";
const QString OAUTH_REDIRECT_URI_QUERY_KEY = "redirect_uri";

QUrl DomainServer::oauthAuthorizationURL(const QUuid& stateUUID) {
    // for now these are all interface clients that have a GUI
    // so just send them back the full authorization URL
    QUrl authorizationURL = _oauthProviderURL;

    const QString OAUTH_AUTHORIZATION_PATH = "/oauth/authorize";
    authorizationURL.setPath(OAUTH_AUTHORIZATION_PATH);

    QUrlQuery authorizationQuery;

    authorizationQuery.addQueryItem(OAUTH_CLIENT_ID_QUERY_KEY, _oauthClientID);

    const QString OAUTH_RESPONSE_TYPE_QUERY_KEY = "response_type";
    const QString OAUTH_REPSONSE_TYPE_QUERY_VALUE = "code";
    authorizationQuery.addQueryItem(OAUTH_RESPONSE_TYPE_QUERY_KEY, OAUTH_REPSONSE_TYPE_QUERY_VALUE);

    const QString OAUTH_STATE_QUERY_KEY = "state";
    // create a new UUID that will be the state parameter for oauth authorization AND the new session UUID for that node
    authorizationQuery.addQueryItem(OAUTH_STATE_QUERY_KEY, uuidStringWithoutCurlyBraces(stateUUID));

    authorizationQuery.addQueryItem(OAUTH_REDIRECT_URI_QUERY_KEY, oauthRedirectURL().toString());

    authorizationURL.setQuery(authorizationQuery);

    return authorizationURL;
}

void DomainServer::handleConnectedNode(SharedNodePointer newNode) {
    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(newNode->getLinkedData());

    // reply back to the user with a PacketType::DomainList
    sendDomainListToNode(newNode, nodeData->getSendingSockAddr());

    // if this node is a user (unassigned Agent), signal
    if (newNode->getType() == NodeType::Agent && !nodeData->wasAssigned()) {
        emit userConnected();
    }

    if (shouldReplicateNode(*newNode)) {
        qDebug() << "Setting node to replicated: " << newNode->getUUID();
        newNode->setIsReplicated(true);
    }

    // send out this node to our other connected nodes
    broadcastNewNode(newNode);
}

void DomainServer::sendDomainListToNode(const SharedNodePointer& node, const HifiSockAddr &senderSockAddr) {
    const int NUM_DOMAIN_LIST_EXTENDED_HEADER_BYTES = NUM_BYTES_RFC4122_UUID + NLPacket::NUM_BYTES_LOCALID + 
        NUM_BYTES_RFC4122_UUID + NLPacket::NUM_BYTES_LOCALID + 4;

    // setup the extended header for the domain list packets
    // this data is at the beginning of each of the domain list packets
    QByteArray extendedHeader(NUM_DOMAIN_LIST_EXTENDED_HEADER_BYTES, 0);
    QDataStream extendedHeaderStream(&extendedHeader, QIODevice::WriteOnly);

    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

    extendedHeaderStream << limitedNodeList->getSessionUUID();
    extendedHeaderStream << limitedNodeList->getSessionLocalID();
    extendedHeaderStream << node->getUUID();
    extendedHeaderStream << node->getLocalID();
    extendedHeaderStream << node->getPermissions();

    auto domainListPackets = NLPacketList::create(PacketType::DomainList, extendedHeader);

    // always send the node their own UUID back
    QDataStream domainListStream(domainListPackets.get());

    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());

    // store the nodeInterestSet on this DomainServerNodeData, in case it has changed
    auto& nodeInterestSet = nodeData->getNodeInterestSet();

    if (nodeInterestSet.size() > 0) {

        // DTLSServerSession* dtlsSession = _isUsingDTLS ? _dtlsSessions[senderSockAddr] : NULL;
        if (nodeData->isAuthenticated()) {
            // if this authenticated node has any interest types, send back those nodes as well
            limitedNodeList->eachNode([&](const SharedNodePointer& otherNode) {
                if (otherNode->getUUID() != node->getUUID() && isInInterestSet(node, otherNode)) {
                    // since we're about to add a node to the packet we start a segment
                    domainListPackets->startSegment();

                    // don't send avatar nodes to other avatars, that will come from avatar mixer
                    domainListStream << *otherNode.data();

                    // pack the secret that these two nodes will use to communicate with each other
                    domainListStream << connectionSecretForNodes(node, otherNode);

                    // we've added the node we wanted so end the segment now
                    domainListPackets->endSegment();
                }
            });
        }
    }

    // send an empty list to the node, in case there were no other nodes
    domainListPackets->closeCurrentPacket(true);

    // write the PacketList to this node
    limitedNodeList->sendPacketList(std::move(domainListPackets), *node);
}

QUuid DomainServer::connectionSecretForNodes(const SharedNodePointer& nodeA, const SharedNodePointer& nodeB) {
    DomainServerNodeData* nodeAData = static_cast<DomainServerNodeData*>(nodeA->getLinkedData());
    DomainServerNodeData* nodeBData = static_cast<DomainServerNodeData*>(nodeB->getLinkedData());

    if (nodeAData && nodeBData) {
        QUuid& secretUUID = nodeAData->getSessionSecretHash()[nodeB->getUUID()];

        if (secretUUID.isNull()) {
            // generate a new secret UUID these two nodes can use
            secretUUID = QUuid::createUuid();

            // set it on the other Node's sessionSecretHash
            static_cast<DomainServerNodeData*>(nodeBData)->getSessionSecretHash().insert(nodeA->getUUID(), secretUUID);
        }

        return secretUUID;
    }

    return QUuid();
}

void DomainServer::broadcastNewNode(const SharedNodePointer& addedNode) {

    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

    auto addNodePacket = NLPacket::create(PacketType::DomainServerAddedNode);

    // setup the add packet for this new node
    QDataStream addNodeStream(addNodePacket.get());

    addNodeStream << *addedNode.data();

    int connectionSecretIndex = addNodePacket->pos();

    limitedNodeList->eachMatchingNode(
        [&](const SharedNodePointer& node)->bool {
            if (node->getLinkedData() && node->getActiveSocket() && node != addedNode) {
                // is the added Node in this node's interest list?
                return isInInterestSet(node, addedNode);
            } else {
                return false;
            }
        },
        [&](const SharedNodePointer& node) {
            addNodePacket->seek(connectionSecretIndex);

            QByteArray rfcConnectionSecret = connectionSecretForNodes(node, addedNode).toRfc4122();

            // replace the bytes at the end of the packet for the connection secret between these nodes
            addNodePacket->write(rfcConnectionSecret);

            // send off this packet to the node
            limitedNodeList->sendUnreliablePacket(*addNodePacket, *node);
        }
    );
}

void DomainServer::processRequestAssignmentPacket(QSharedPointer<ReceivedMessage> message) {
    // construct the requested assignment from the packet data
    Assignment requestAssignment(*message);

    auto senderAddr = message->getSenderSockAddr().getAddress();

    auto isHostAddressInSubnet = [&senderAddr](const Subnet& mask) -> bool {
        return senderAddr.isInSubnet(mask);
    };

    auto it = find_if(_acSubnetWhitelist.begin(), _acSubnetWhitelist.end(), isHostAddressInSubnet);
    if (it == _acSubnetWhitelist.end()) {
        HIFI_FDEBUG("Received an assignment connect request from a disallowed ip address:"
            << senderAddr.toString());
        return;
    }

    static bool printedAssignmentTypeMessage = false;
    if (!printedAssignmentTypeMessage && requestAssignment.getType() != Assignment::AgentType) {
        printedAssignmentTypeMessage = true;
        qDebug() << "Received a request for assignment type" << requestAssignment.getType()
                 << "from" << message->getSenderSockAddr();
    }

    SharedAssignmentPointer assignmentToDeploy = deployableAssignmentForRequest(requestAssignment);

    if (assignmentToDeploy) {
        qDebug() << "Deploying assignment -" << *assignmentToDeploy.data() << "- to" << message->getSenderSockAddr();

        // give this assignment out, either the type matches or the requestor said they will take any
        static std::unique_ptr<NLPacket> assignmentPacket;

        if (!assignmentPacket) {
            assignmentPacket = NLPacket::create(PacketType::CreateAssignment);
        }

        // setup a copy of this assignment that will have a unique UUID, for packaging purposes
        Assignment uniqueAssignment(*assignmentToDeploy.data());
        uniqueAssignment.setUUID(QUuid::createUuid());

        // reset the assignmentPacket
        assignmentPacket->reset();

        QDataStream assignmentStream(assignmentPacket.get());

        assignmentStream << uniqueAssignment;

        auto limitedNodeList = DependencyManager::get<LimitedNodeList>();
        limitedNodeList->sendUnreliablePacket(*assignmentPacket, message->getSenderSockAddr());

        // give the information for that deployed assignment to the gatekeeper so it knows to that that node
        // in when it comes back around
        _gatekeeper.addPendingAssignedNode(uniqueAssignment.getUUID(), assignmentToDeploy->getUUID(),
                                           requestAssignment.getWalletUUID(), requestAssignment.getNodeVersion());
    } else {
        static bool printedAssignmentRequestMessage = false;
        if (!printedAssignmentRequestMessage && requestAssignment.getType() != Assignment::AgentType) {
            printedAssignmentRequestMessage = true;
            qDebug() << "Unable to fulfill assignment request of type" << requestAssignment.getType()
                << "from" << message->getSenderSockAddr();
        }
    }
}

void DomainServer::setupPendingAssignmentCredits() {
    // enumerate the NodeList to find the assigned nodes
    DependencyManager::get<LimitedNodeList>()->eachNode([&](const SharedNodePointer& node){
        DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());

        if (!nodeData->getAssignmentUUID().isNull() && !nodeData->getWalletUUID().isNull()) {
            // check if we have a non-finalized transaction for this node to add this amount to
            TransactionHash::iterator i = _pendingAssignmentCredits.find(nodeData->getWalletUUID());
            WalletTransaction* existingTransaction = NULL;

            while (i != _pendingAssignmentCredits.end() && i.key() == nodeData->getWalletUUID()) {
                if (!i.value()->isFinalized()) {
                    existingTransaction = i.value();
                    break;
                } else {
                    ++i;
                }
            }

            qint64 elapsedMsecsSinceLastPayment = nodeData->getPaymentIntervalTimer().elapsed();
            nodeData->getPaymentIntervalTimer().restart();

            const float CREDITS_PER_HOUR = 0.10f;
            const float CREDITS_PER_MSEC = CREDITS_PER_HOUR / (60 * 60 * 1000);
            const int SATOSHIS_PER_MSEC = CREDITS_PER_MSEC * SATOSHIS_PER_CREDIT;

            float pendingCredits = elapsedMsecsSinceLastPayment * SATOSHIS_PER_MSEC;

            if (existingTransaction) {
                existingTransaction->incrementAmount(pendingCredits);
            } else {
                // create a fresh transaction to pay this node, there is no transaction to append to
                WalletTransaction* freshTransaction = new WalletTransaction(nodeData->getWalletUUID(), pendingCredits);
                _pendingAssignmentCredits.insert(nodeData->getWalletUUID(), freshTransaction);
            }
        }
    });
}

void DomainServer::sendPendingTransactionsToServer() {

    auto accountManager = DependencyManager::get<AccountManager>();

    if (accountManager->hasValidAccessToken()) {

        // enumerate the pending transactions and send them to the server to complete payment
        TransactionHash::iterator i = _pendingAssignmentCredits.begin();

        JSONCallbackParameters transactionCallbackParams;

        transactionCallbackParams.jsonCallbackReceiver = this;
        transactionCallbackParams.jsonCallbackMethod = "transactionJSONCallback";

        while (i != _pendingAssignmentCredits.end()) {
            accountManager->sendRequest("api/v1/transactions",
                                       AccountManagerAuth::Required,
                                       QNetworkAccessManager::PostOperation,
                                       transactionCallbackParams, i.value()->postJson().toJson());

            // set this transaction to finalized so we don't add additional credits to it
            i.value()->setIsFinalized(true);

            ++i;
        }
    }
}

void DomainServer::transactionJSONCallback(const QJsonObject& data) {
    // check if this was successful - if so we can remove it from our list of pending
    if (data.value("status").toString() == "success") {
        // create a dummy wallet transaction to unpack the JSON to
        WalletTransaction dummyTransaction;
        dummyTransaction.loadFromJson(data);

        TransactionHash::iterator i = _pendingAssignmentCredits.find(dummyTransaction.getDestinationUUID());

        while (i != _pendingAssignmentCredits.end() && i.key() == dummyTransaction.getDestinationUUID()) {
            if (i.value()->getUUID() == dummyTransaction.getUUID()) {
                // we have a match - we can remove this from the hash of pending credits
                // and delete it for clean up

                WalletTransaction* matchingTransaction = i.value();
                _pendingAssignmentCredits.erase(i);
                delete matchingTransaction;

                break;
            } else {
                ++i;
            }
        }
    }
}

QJsonObject jsonForDomainSocketUpdate(const HifiSockAddr& socket) {
    const QString SOCKET_NETWORK_ADDRESS_KEY = "network_address";
    const QString SOCKET_PORT_KEY = "port";

    QJsonObject socketObject;
    socketObject[SOCKET_NETWORK_ADDRESS_KEY] = socket.getAddress().toString();
    socketObject[SOCKET_PORT_KEY] = socket.getPort();

    return socketObject;
}

const QString DOMAIN_UPDATE_AUTOMATIC_NETWORKING_KEY = "automatic_networking";

void DomainServer::performIPAddressUpdate(const HifiSockAddr& newPublicSockAddr) {
    sendHeartbeatToMetaverse(newPublicSockAddr.getAddress().toString());
}

void DomainServer::sendHeartbeatToMetaverse(const QString& networkAddress) {
    // Setup the domain object to send to the data server
    QJsonObject domainObject;

    // add the versions
    static const QString VERSION_KEY = "version";
    domainObject[VERSION_KEY] = BuildInfo::VERSION;
    static const QString PROTOCOL_VERSION_KEY = "protocol";
    domainObject[PROTOCOL_VERSION_KEY] = protocolVersionsSignatureBase64();

    // add networking
    if (!networkAddress.isEmpty()) {
        static const QString PUBLIC_NETWORK_ADDRESS_KEY = "network_address";
        domainObject[PUBLIC_NETWORK_ADDRESS_KEY] = networkAddress;
    }

    static const QString AUTOMATIC_NETWORKING_KEY = "automatic_networking";
    domainObject[AUTOMATIC_NETWORKING_KEY] = _automaticNetworkingSetting;

    // add access level for anonymous connections
    // consider the domain to be "restricted" if anonymous connections are disallowed
    static const QString RESTRICTED_ACCESS_FLAG = "restricted";
    NodePermissions anonymousPermissions = _settingsManager.getPermissionsForName(NodePermissions::standardNameAnonymous);
    domainObject[RESTRICTED_ACCESS_FLAG] = !anonymousPermissions.can(NodePermissions::Permission::canConnectToDomain);

    const auto& temporaryDomainKey = DependencyManager::get<AccountManager>()->getTemporaryDomainKey(getID());
    if (!temporaryDomainKey.isEmpty()) {
        // add the temporary domain token
        const QString KEY_KEY = "api_key";
        domainObject[KEY_KEY] = temporaryDomainKey;
    }

    if (_metadata) {
        // Add the metadata to the heartbeat
        static const QString DOMAIN_HEARTBEAT_KEY = "heartbeat";
        domainObject[DOMAIN_HEARTBEAT_KEY] = _metadata->get(DomainMetadata::USERS);
    }

    QString domainUpdateJSON = QString("{\"domain\":%1}").arg(QString(QJsonDocument(domainObject).toJson(QJsonDocument::Compact)));

    static const QString DOMAIN_UPDATE = "/api/v1/domains/%1";
    DependencyManager::get<AccountManager>()->sendRequest(DOMAIN_UPDATE.arg(uuidStringWithoutCurlyBraces(getID())),
                                              AccountManagerAuth::Optional,
                                              QNetworkAccessManager::PutOperation,
                                              JSONCallbackParameters(nullptr, QString(), this, "handleMetaverseHeartbeatError"),
                                              domainUpdateJSON.toUtf8());
}

void DomainServer::handleMetaverseHeartbeatError(QNetworkReply& requestReply) {
    if (!_metaverseHeartbeatTimer) {
        // avoid rehandling errors from the same issue
        return;
    }

    // only attempt to grab a new temporary name if we're already a temporary domain server
    if (_type == MetaverseTemporaryDomain) {
        // check if we need to force a new temporary domain name
        switch (requestReply.error()) {
                // if we have a temporary domain with a bad token, we get a 401
            case QNetworkReply::NetworkError::AuthenticationRequiredError: {
                static const QString DATA_KEY = "data";
                static const QString TOKEN_KEY = "api_key";

                QJsonObject jsonObject = QJsonDocument::fromJson(requestReply.readAll()).object();
                auto tokenFailure = jsonObject[DATA_KEY].toObject()[TOKEN_KEY];

                if (!tokenFailure.isNull()) {
                    qWarning() << "Temporary domain name lacks a valid API key, and is being reset.";
                }
                break;
            }
                // if the domain does not (or no longer) exists, we get a 404
            case QNetworkReply::NetworkError::ContentNotFoundError:
                qWarning() << "Domain not found, getting a new temporary domain.";
                break;
                // otherwise, we erred on something else, and should not force a temporary domain
            default:
                return;
        }

        // halt heartbeats until we have a token
        _metaverseHeartbeatTimer->deleteLater();
        _metaverseHeartbeatTimer = nullptr;

        // give up eventually to avoid flooding traffic
        static const int MAX_ATTEMPTS = 5;
        static int attempt = 0;
        if (++attempt < MAX_ATTEMPTS) {
            // get a new temporary name and token
            getTemporaryName(true);
        } else {
            qWarning() << "Already attempted too many temporary domain requests. Please set a domain ID manually or restart.";
        }
    }
}

void DomainServer::sendICEServerAddressToMetaverseAPI() {
    if (_sendICEServerAddressToMetaverseAPIInProgress) {
        // don't have more than one of these in-flight at a time.  set a flag to indicate that once the current one
        // is done, we need to do update metaverse again.
        _sendICEServerAddressToMetaverseAPIRedo = true;
        return;
    }
    _sendICEServerAddressToMetaverseAPIInProgress = true;
    const QString ICE_SERVER_ADDRESS = "ice_server_address";

    QJsonObject domainObject;

    if (!_connectedToICEServer || _iceServerSocket.isNull()) {
        domainObject[ICE_SERVER_ADDRESS] = "0.0.0.0";
    } else {
        // we're using full automatic networking and we have a current ice-server socket, use that now
        domainObject[ICE_SERVER_ADDRESS] = _iceServerSocket.getAddress().toString();
    }

    const auto& temporaryDomainKey = DependencyManager::get<AccountManager>()->getTemporaryDomainKey(getID());
    if (!temporaryDomainKey.isEmpty()) {
        // add the temporary domain token
        const QString KEY_KEY = "api_key";
        domainObject[KEY_KEY] = temporaryDomainKey;
    }

    QString domainUpdateJSON = QString("{\"domain\": %1 }").arg(QString(QJsonDocument(domainObject).toJson()));

    // make sure we hear about failure so we can retry
    JSONCallbackParameters callbackParameters;
    callbackParameters.errorCallbackReceiver = this;
    callbackParameters.errorCallbackMethod = "handleFailedICEServerAddressUpdate";
    callbackParameters.jsonCallbackReceiver = this;
    callbackParameters.jsonCallbackMethod = "handleSuccessfulICEServerAddressUpdate";

    static bool printedIceServerMessage = false;
    if (!printedIceServerMessage) {
        printedIceServerMessage = true;
        qDebug() << "Updating ice-server address in High Fidelity Metaverse API to"
            << (_iceServerSocket.isNull() ? "" : _iceServerSocket.getAddress().toString());
    }

    static const QString DOMAIN_ICE_ADDRESS_UPDATE = "/api/v1/domains/%1/ice_server_address";

    DependencyManager::get<AccountManager>()->sendRequest(DOMAIN_ICE_ADDRESS_UPDATE.arg(uuidStringWithoutCurlyBraces(getID())),
                                                          AccountManagerAuth::Optional,
                                                          QNetworkAccessManager::PutOperation,
                                                          callbackParameters,
                                                          domainUpdateJSON.toUtf8());
}

void DomainServer::handleSuccessfulICEServerAddressUpdate(QNetworkReply& requestReply) {
    _sendICEServerAddressToMetaverseAPIInProgress = false;
    if (_sendICEServerAddressToMetaverseAPIRedo) {
        qDebug() << "ice-server address updated with metaverse, but has since changed.  redoing update...";
        _sendICEServerAddressToMetaverseAPIRedo = false;
        sendICEServerAddressToMetaverseAPI();
    } else {
        qDebug() << "ice-server address updated with metaverse.";
    }
}

void DomainServer::handleFailedICEServerAddressUpdate(QNetworkReply& requestReply) {
    _sendICEServerAddressToMetaverseAPIInProgress = false;
    if (_sendICEServerAddressToMetaverseAPIRedo) {
        // if we have new data, retry right away, even though the previous attempt didn't go well.
        _sendICEServerAddressToMetaverseAPIRedo = false;
        sendICEServerAddressToMetaverseAPI();
    } else {
        const int ICE_SERVER_UPDATE_RETRY_MS = 2 * 1000;

        qWarning() << "Failed to update ice-server address with High Fidelity Metaverse - error was"
                   << requestReply.errorString();
        qWarning() << "\tRe-attempting in" << ICE_SERVER_UPDATE_RETRY_MS / 1000 << "seconds";

        QTimer::singleShot(ICE_SERVER_UPDATE_RETRY_MS, this, SLOT(sendICEServerAddressToMetaverseAPI()));
    }
}

void DomainServer::sendHeartbeatToIceServer() {
    if (!_iceServerSocket.getAddress().isNull()) {

        auto accountManager = DependencyManager::get<AccountManager>();
        auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

        if (!accountManager->getAccountInfo().hasPrivateKey()) {
            qWarning() << "Cannot send an ice-server heartbeat without a private key for signature.";
            qWarning() << "Waiting for keypair generation to complete before sending ICE heartbeat.";

            if (!limitedNodeList->getSessionUUID().isNull()) {
                accountManager->generateNewDomainKeypair(limitedNodeList->getSessionUUID());
            } else {
                qWarning() << "Attempting to send ICE server heartbeat with no domain ID. This is not supported";
            }

            return;
        }

        const int FAILOVER_NO_REPLY_ICE_HEARTBEATS { 3 };

        // increase the count of no reply ICE heartbeats and check the current value
        ++_noReplyICEHeartbeats;

        if (_noReplyICEHeartbeats > FAILOVER_NO_REPLY_ICE_HEARTBEATS) {
            qWarning() << "There have been" << _noReplyICEHeartbeats - 1 << "heartbeats sent with no reply from the ice-server";
            qWarning() << "Clearing the current ice-server socket and selecting a new candidate ice-server";

            // add the current address to our list of failed addresses
            _failedIceServerAddresses << _iceServerSocket.getAddress();

            // if we've failed to hear back for three heartbeats, we clear the current ice-server socket and attempt
            // to randomize a new one
            _iceServerSocket.clear();

            // reset the number of no reply ICE hearbeats
            _noReplyICEHeartbeats = 0;

            // reset the connection flag for ICE server
            _connectedToICEServer = false;
            sendICEServerAddressToMetaverseAPI();

            // randomize our ice-server address (and simultaneously look up any new hostnames for available ice-servers)
            randomizeICEServerAddress(true);
        }

        // NOTE: I'd love to specify the correct size for the packet here, but it's a little trickey with
        // QDataStream and the possibility of IPv6 address for the sockets.
        if (!_iceServerHeartbeatPacket) {
            _iceServerHeartbeatPacket = NLPacket::create(PacketType::ICEServerHeartbeat);
        }

        bool shouldRecreatePacket = false;

        if (_iceServerHeartbeatPacket->getPayloadSize() > 0) {
            // if either of our sockets have changed we need to re-sign the heartbeat
            // first read the sockets out from the current packet
            _iceServerHeartbeatPacket->seek(0);
            QDataStream heartbeatStream(_iceServerHeartbeatPacket.get());

            QUuid senderUUID;
            HifiSockAddr publicSocket, localSocket;
            heartbeatStream >> senderUUID >> publicSocket >> localSocket;

            if (senderUUID != limitedNodeList->getSessionUUID()
                || publicSocket != limitedNodeList->getPublicSockAddr()
                || localSocket != limitedNodeList->getLocalSockAddr()) {
                shouldRecreatePacket = true;
            }
        } else {
            shouldRecreatePacket = true;
        }

        if (shouldRecreatePacket) {
            // either we don't have a heartbeat packet yet or some combination of sockets, ID and keypair have changed
            // and we need to make a new one

            // reset the position in the packet before writing
            _iceServerHeartbeatPacket->reset();

            // write our plaintext data to the packet
            QDataStream heartbeatDataStream(_iceServerHeartbeatPacket.get());
            heartbeatDataStream << limitedNodeList->getSessionUUID()
                << limitedNodeList->getPublicSockAddr() << limitedNodeList->getLocalSockAddr();

            // setup a QByteArray that points to the plaintext data
            auto plaintext = QByteArray::fromRawData(_iceServerHeartbeatPacket->getPayload(), _iceServerHeartbeatPacket->getPayloadSize());

            // generate a signature for the plaintext data in the packet
            auto signature = accountManager->getAccountInfo().signPlaintext(plaintext);

            // pack the signature with the data
            heartbeatDataStream << signature;
        }

        // send the heartbeat packet to the ice server now
        limitedNodeList->sendUnreliablePacket(*_iceServerHeartbeatPacket, _iceServerSocket);

    } else {
        qDebug() << "Not sending ice-server heartbeat since there is no selected ice-server.";
        qDebug() << "Waiting for" << _iceServerAddr << "host lookup response";
    }
}

void DomainServer::processOctreeDataPersistMessage(QSharedPointer<ReceivedMessage> message) {
    qDebug() << "Received octree data persist message";
    auto data = message->readAll();
    auto filePath = getEntitiesFilePath();

    QDir dir(getEntitiesDirPath());
    if (!dir.exists()) {
        qCDebug(domain_server) << "Creating entities content directory:" << dir.absolutePath();
        dir.mkpath(".");
    }

    QFile f(filePath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
        OctreeUtils::RawEntityData entityData;
        if (entityData.readOctreeDataInfoFromData(data)) {
            qCDebug(domain_server) << "Wrote new entities file" << entityData.id << entityData.version;
        } else {
            qCDebug(domain_server) << "Failed to read new octree data info";
        }
    } else {
        qCDebug(domain_server) << "Failed to write new entities file:" << filePath;
    }
}

QString DomainServer::getContentBackupDir() {
    return PathUtils::getAppDataFilePath("backups");
}

QString DomainServer::getEntitiesDirPath() {
    return PathUtils::getAppDataFilePath("entities");
}

QString DomainServer::getEntitiesFilePath() {
    return PathUtils::getAppDataFilePath("entities/models.json.gz");
}

QString DomainServer::getEntitiesReplacementFilePath() {
    return getEntitiesFilePath().append(REPLACEMENT_FILE_EXTENSION);
}

void DomainServer::processOctreeDataRequestMessage(QSharedPointer<ReceivedMessage> message) {
    qDebug() << "Got request for octree data from " << message->getSenderSockAddr();

    maybeHandleReplacementEntityFile();

    bool remoteHasExistingData { false };
    QUuid id;
    int version;
    message->readPrimitive(&remoteHasExistingData);
    if (remoteHasExistingData) {
        constexpr size_t UUID_SIZE_BYTES = 16;
        auto idData = message->read(UUID_SIZE_BYTES);
        id = QUuid::fromRfc4122(idData);
        message->readPrimitive(&version);
        qCDebug(domain_server) << "Entity server does have existing data: ID(" << id << ") DataVersion(" << version << ")";
    } else {
        qCDebug(domain_server) << "Entity server does not have existing data";
    }
    auto entityFilePath = getEntitiesFilePath();

    auto reply = NLPacketList::create(PacketType::OctreeDataFileReply, QByteArray(), true, true);
    OctreeUtils::RawEntityData data;
    if (data.readOctreeDataInfoFromFile(entityFilePath)) {
        if (data.id == id && data.version <= version) {
            qCDebug(domain_server) << "ES has sufficient octree data, not sending data";
            reply->writePrimitive(false);
        } else {
            qCDebug(domain_server) << "Sending newer octree data to ES: ID(" << data.id << ") DataVersion(" << data.version << ")";
            QFile file(entityFilePath);
            if (file.open(QIODevice::ReadOnly)) {
                reply->writePrimitive(true);
                reply->write(file.readAll());
            } else {
                qCDebug(domain_server) << "Unable to load entity file";
                reply->writePrimitive(false);
            }
        }
    } else {
        qCDebug(domain_server) << "Domain server does not have valid octree data";
        reply->writePrimitive(false);
    }

    auto nodeList = DependencyManager::get<LimitedNodeList>();
    nodeList->sendPacketList(std::move(reply), message->getSenderSockAddr());
}

void DomainServer::processNodeJSONStatsPacket(QSharedPointer<ReceivedMessage> packetList, SharedNodePointer sendingNode) {
    auto nodeData = static_cast<DomainServerNodeData*>(sendingNode->getLinkedData());
    if (nodeData) {
        nodeData->updateJSONStats(packetList->getMessage());
    }
}

QJsonObject DomainServer::jsonForSocket(const HifiSockAddr& socket) {
    QJsonObject socketJSON;

    socketJSON["ip"] = socket.getAddress().toString();
    socketJSON["port"] = socket.getPort();

    return socketJSON;
}

const char JSON_KEY_UUID[] = "uuid";
const char JSON_KEY_TYPE[] = "type";
const char JSON_KEY_PUBLIC_SOCKET[] = "public";
const char JSON_KEY_LOCAL_SOCKET[] = "local";
const char JSON_KEY_POOL[] = "pool";
const char JSON_KEY_PENDING_CREDITS[] = "pending_credits";
const char JSON_KEY_UPTIME[] = "uptime";
const char JSON_KEY_USERNAME[] = "username";
const char JSON_KEY_VERSION[] = "version";
QJsonObject DomainServer::jsonObjectForNode(const SharedNodePointer& node) {
    QJsonObject nodeJson;

    // re-format the type name so it matches the target name
    QString nodeTypeName = NodeType::getNodeTypeName(node->getType());
    nodeTypeName = nodeTypeName.toLower();
    nodeTypeName.replace(' ', '-');

    // add the node UUID
    nodeJson[JSON_KEY_UUID] = uuidStringWithoutCurlyBraces(node->getUUID());

    // add the node type
    nodeJson[JSON_KEY_TYPE] = nodeTypeName;

    // add the node socket information
    nodeJson[JSON_KEY_PUBLIC_SOCKET] = jsonForSocket(node->getPublicSocket());
    nodeJson[JSON_KEY_LOCAL_SOCKET] = jsonForSocket(node->getLocalSocket());

    // add the node uptime in our list
    nodeJson[JSON_KEY_UPTIME] = QString::number(double(QDateTime::currentMSecsSinceEpoch() - node->getWakeTimestamp()) / 1000.0);

    // if the node has pool information, add it
    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());

    // add the node username, if it exists
    nodeJson[JSON_KEY_USERNAME] = nodeData->getUsername();
    nodeJson[JSON_KEY_VERSION] = nodeData->getNodeVersion();

    SharedAssignmentPointer matchingAssignment = _allAssignments.value(nodeData->getAssignmentUUID());
    if (matchingAssignment) {
        nodeJson[JSON_KEY_POOL] = matchingAssignment->getPool();

        if (!nodeData->getWalletUUID().isNull()) {
            TransactionHash::iterator i = _pendingAssignmentCredits.find(nodeData->getWalletUUID());
            float pendingCreditAmount = 0;

            while (i != _pendingAssignmentCredits.end() && i.key() == nodeData->getWalletUUID()) {
                pendingCreditAmount += i.value()->getAmount() / SATOSHIS_PER_CREDIT;
                ++i;
            }

            nodeJson[JSON_KEY_PENDING_CREDITS] = pendingCreditAmount;
        }
    }

    return nodeJson;
}

QDir pathForAssignmentScriptsDirectory() {
    static const QString SCRIPTS_DIRECTORY_NAME = "/scripts/";

    QDir directory(PathUtils::getAppDataPath() + SCRIPTS_DIRECTORY_NAME);
    if (!directory.exists()) {
        directory.mkpath(".");
        qInfo() << "Created path to " << directory.path();
    }

    return directory;
}

QString pathForAssignmentScript(const QUuid& assignmentUUID) {
    QDir directory = pathForAssignmentScriptsDirectory();
    // append the UUID for this script as the new filename, remove the curly braces
    return directory.absoluteFilePath(uuidStringWithoutCurlyBraces(assignmentUUID));
}

QString DomainServer::pathForRedirect(QString path) const {
    // make sure the passed path has a leading slash
    if (!path.startsWith('/')) {
        path.insert(0, '/');
    }

    return "http://" + _hostname + ":" + QString::number(_httpManager.serverPort()) + path;
}



const QString URI_OAUTH = "/oauth";
bool DomainServer::handleHTTPRequest(HTTPConnection* connection, const QUrl& url, bool skipSubHandler) {
    const QString JSON_MIME_TYPE = "application/json";

    const QString URI_ASSIGNMENT = "/assignment";
    const QString URI_NODES = "/nodes";
    const QString URI_SETTINGS = "/settings";
    const QString URI_CONTENT_UPLOAD = "/content/upload";
    const QString URI_RESTART = "/restart";
    const QString URI_API_PLACES = "/api/places";
    const QString URI_API_DOMAINS = "/api/domains";
    const QString URI_API_DOMAINS_ID = "/api/domains/";
    const QString URI_API_BACKUPS = "/api/backups";
    const QString URI_API_BACKUPS_ID = "/api/backups/";
    const QString URI_API_BACKUPS_DOWNLOAD_ID = "/api/backups/download/";
    const QString URI_API_BACKUPS_RECOVER = "/api/backups/recover/";

    const QString UUID_REGEX_STRING = "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";

    QPointer<HTTPConnection> connectionPtr { connection };

    auto nodeList = DependencyManager::get<LimitedNodeList>();

    auto getSetting = [this](QString keyPath, QVariant& value) -> bool {

        value = _settingsManager.valueForKeyPath(keyPath);
        if (!value.isValid()) {
            return false;
        }
        return true;
    };

    // check if this is a request for a scripted assignment (with a temp unique UUID)
    const QString ASSIGNMENT_REGEX_STRING = QString("\\%1\\/(%2)\\/?$").arg(URI_ASSIGNMENT).arg(UUID_REGEX_STRING);
    QRegExp assignmentRegex(ASSIGNMENT_REGEX_STRING);

    if (connection->requestOperation() == QNetworkAccessManager::GetOperation
        && assignmentRegex.indexIn(url.path()) != -1) {
        QUuid nodeUUID = QUuid(assignmentRegex.cap(1));

        auto matchingNode = nodeList->nodeWithUUID(nodeUUID);

        // don't handle if we don't have a matching node
        if (!matchingNode) {
            return false;
        }

        auto nodeData = static_cast<DomainServerNodeData*>(matchingNode->getLinkedData());

        // don't handle if we don't have node data for this node
        if (!nodeData) {
            return false;
        }

        SharedAssignmentPointer matchingAssignment = _allAssignments.value(nodeData->getAssignmentUUID());

        // check if we have an assignment that matches this temp UUID, and it is a scripted assignment
        if (matchingAssignment && matchingAssignment->getType() == Assignment::AgentType) {
            // we have a matching assignment and it is for the right type, have the HTTP manager handle it
            // via correct URL for the script so the client can download
            const auto it = _ephemeralACScripts.find(matchingAssignment->getUUID());

            if (it != _ephemeralACScripts.end()) {
                connection->respond(HTTPConnection::StatusCode200, it->second, "application/javascript");
            } else {
                connection->respond(HTTPConnection::StatusCode404, "Resource not found.");
            }

            return true;
        }

        // request not handled
        return false;
    }

    // check if this is a request for our domain ID
    const QString URI_ID = "/id";
    if (connection->requestOperation() == QNetworkAccessManager::GetOperation
        && url.path() == URI_ID) {
        QUuid domainID = nodeList->getSessionUUID();

        connection->respond(HTTPConnection::StatusCode200, uuidStringWithoutCurlyBraces(domainID).toLocal8Bit());
        return true;
    }

    // all requests below require a cookie to prove authentication so check that first
    if (!isAuthenticatedRequest(connection, url)) {
        // this is not an authenticated request
        // return true from the handler since it was handled with a 401 or re-direct to auth
        return true;
    }

    // Check if we should redirect/prevent access to the wizard
    if (connection->requestOperation() == QNetworkAccessManager::GetOperation) {
        const QString URI_WIZARD = "/wizard/";
        const QString WIZARD_COMPLETED_ONCE_KEY_PATH = "wizard.completed_once";
        QVariant wizardCompletedOnce = _settingsManager.valueForKeyPath(WIZARD_COMPLETED_ONCE_KEY_PATH);
        const bool completedOnce = wizardCompletedOnce.isValid() && wizardCompletedOnce.toBool();

        if (url.path() != URI_WIZARD && url.path().endsWith('/') && !completedOnce) {
            // First visit, redirect to the wizard
            QUrl redirectedURL = url;
            redirectedURL.setPath(URI_WIZARD);

            Headers redirectHeaders;
            redirectHeaders.insert("Location", redirectedURL.toEncoded());

            connection->respond(HTTPConnection::StatusCode302,
                                QByteArray(), HTTPConnection::DefaultContentType, redirectHeaders);
            return true;
        } else if (url.path() == URI_WIZARD && completedOnce) {
            // Wizard already completed, return 404
            connection->respond(HTTPConnection::StatusCode404, "Resource not found.");
            return true;
        }
    }

    if (connection->requestOperation() == QNetworkAccessManager::GetOperation) {
        if (url.path() == "/assignments.json") {
            // user is asking for json list of assignments

            // setup the JSON
            QJsonObject assignmentJSON;
            QJsonObject assignedNodesJSON;

            // enumerate the NodeList to find the assigned nodes
            nodeList->eachNode([this, &assignedNodesJSON](const SharedNodePointer& node){
                DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());

                if (!nodeData->getAssignmentUUID().isNull()) {
                    // add the node using the UUID as the key
                    QString uuidString = uuidStringWithoutCurlyBraces(nodeData->getAssignmentUUID());
                    assignedNodesJSON[uuidString] = jsonObjectForNode(node);
                }
            });

            assignmentJSON["fulfilled"] = assignedNodesJSON;

            QJsonObject queuedAssignmentsJSON;

            // add the queued but unfilled assignments to the json
            foreach(const SharedAssignmentPointer& assignment, _unfulfilledAssignments) {
                QJsonObject queuedAssignmentJSON;

                QString uuidString = uuidStringWithoutCurlyBraces(assignment->getUUID());
                queuedAssignmentJSON[JSON_KEY_TYPE] = QString(assignment->getTypeName());

                // if the assignment has a pool, add it
                if (!assignment->getPool().isEmpty()) {
                    queuedAssignmentJSON[JSON_KEY_POOL] = assignment->getPool();
                }

                // add this queued assignment to the JSON
                queuedAssignmentsJSON[uuidString] = queuedAssignmentJSON;
            }

            assignmentJSON["queued"] = queuedAssignmentsJSON;

            // print out the created JSON
            QJsonDocument assignmentDocument(assignmentJSON);
            connection->respond(HTTPConnection::StatusCode200, assignmentDocument.toJson(), qPrintable(JSON_MIME_TYPE));

            // we've processed this request
            return true;
        } else if (url.path() == "/transactions.json") {
            // enumerate our pending transactions and display them in an array
            QJsonObject rootObject;
            QJsonArray transactionArray;

            TransactionHash::iterator i = _pendingAssignmentCredits.begin();
            while (i != _pendingAssignmentCredits.end()) {
                transactionArray.push_back(i.value()->toJson());
                ++i;
            }

            rootObject["pending_transactions"] = transactionArray;

            // print out the created JSON
            QJsonDocument transactionsDocument(rootObject);
            connection->respond(HTTPConnection::StatusCode200, transactionsDocument.toJson(), qPrintable(JSON_MIME_TYPE));

            return true;
        } else if (url.path() == QString("%1.json").arg(URI_NODES)) {
            // setup the JSON
            QJsonObject rootJSON;
            QJsonArray nodesJSONArray;

            // enumerate the NodeList to find the assigned nodes
            nodeList->eachNode([this, &nodesJSONArray](const SharedNodePointer& node){
                // add the node using the UUID as the key
                nodesJSONArray.append(jsonObjectForNode(node));
            });

            rootJSON["nodes"] = nodesJSONArray;

            // print out the created JSON
            QJsonDocument nodesDocument(rootJSON);

            // send the response
            connection->respond(HTTPConnection::StatusCode200, nodesDocument.toJson(), qPrintable(JSON_MIME_TYPE));

            return true;
        } else if (url.path() == URI_API_BACKUPS) {
            auto deferred = makePromise("getAllBackupsAndStatus");
            deferred->then([connectionPtr, JSON_MIME_TYPE](QString error, QVariantMap result) {
                if (!connectionPtr) {
                    return;
                }

                QJsonDocument docJSON(QJsonObject::fromVariantMap(result));

                connectionPtr->respond(HTTPConnection::StatusCode200, docJSON.toJson(), JSON_MIME_TYPE.toUtf8());
            });
            _contentManager->getAllBackupsAndStatus(deferred);
            return true;
        } else if (url.path().startsWith(URI_API_BACKUPS_DOWNLOAD_ID)) {
            auto id = url.path().mid(QString(URI_API_BACKUPS_DOWNLOAD_ID).length());
            auto info = _contentManager->consolidateBackup(id);

            if (info.state == ConsolidatedBackupInfo::COMPLETE_WITH_SUCCESS) {
                auto file { std::unique_ptr<QFile>(new QFile(info.absoluteFilePath)) };
                if (file->open(QIODevice::ReadOnly)) {
                    constexpr const char* CONTENT_TYPE_ZIP = "application/zip";
                    auto downloadedFilename = id;
                    downloadedFilename.replace(QRegularExpression(".zip$"), ".content.zip");
                    auto contentDisposition = "attachment; filename=\"" + downloadedFilename + "\"";
                    connectionPtr->respond(HTTPConnection::StatusCode200, std::move(file), CONTENT_TYPE_ZIP, {
                        { "Content-Disposition", contentDisposition.toUtf8() }
                    });
                } else {
                    qCritical(domain_server) << "Unable to load consolidated backup at:" << info.absoluteFilePath;
                    connectionPtr->respond(HTTPConnection::StatusCode500, "Error opening backup");
                }
            } else if (info.state == ConsolidatedBackupInfo::COMPLETE_WITH_ERROR) {
                connectionPtr->respond(HTTPConnection::StatusCode500, ("Error creating backup: " + info.error).toUtf8());
            } else {
                connectionPtr->respond(HTTPConnection::StatusCode400, "Backup unavailable");
            }
            return true;
        } else if (url.path().startsWith(URI_API_BACKUPS_ID)) {
            auto id = url.path().mid(QString(URI_API_BACKUPS_ID).length());
            auto info = _contentManager->consolidateBackup(id);

            QJsonObject rootJSON {
                { "complete", info.state == ConsolidatedBackupInfo::COMPLETE_WITH_SUCCESS },
                { "error", info.error }
            };
            QJsonDocument docJSON { rootJSON };
            connectionPtr->respond(HTTPConnection::StatusCode200, docJSON.toJson(), JSON_MIME_TYPE.toUtf8());

            return true;
        } else if (url.path() == URI_RESTART) {
            connection->respond(HTTPConnection::StatusCode200);
            restart();
            return true;
        } else if (url.path() == URI_API_DOMAINS) {
            return forwardMetaverseAPIRequest(connection, "/api/v1/domains", "");
        } else if (url.path().startsWith(URI_API_DOMAINS_ID)) {
            auto id = url.path().mid(URI_API_DOMAINS_ID.length());
            return forwardMetaverseAPIRequest(connection, "/api/v1/domains/" + id, "", {}, {}, false);
        } else if (url.path() == URI_API_PLACES) {
            return forwardMetaverseAPIRequest(connection, "/api/v1/user/places", "");
        } else {
            // check if this is for json stats for a node
            const QString NODE_JSON_REGEX_STRING = QString("\\%1\\/(%2).json\\/?$").arg(URI_NODES).arg(UUID_REGEX_STRING);
            QRegExp nodeShowRegex(NODE_JSON_REGEX_STRING);

            if (nodeShowRegex.indexIn(url.path()) != -1) {
                QUuid matchingUUID = QUuid(nodeShowRegex.cap(1));

                // see if we have a node that matches this ID
                SharedNodePointer matchingNode = nodeList->nodeWithUUID(matchingUUID);
                if (matchingNode) {
                    // create a QJsonDocument with the stats QJsonObject
                    QJsonObject statsObject =
                        static_cast<DomainServerNodeData*>(matchingNode->getLinkedData())->getStatsJSONObject();

                    // add the node type to the JSON data for output purposes
                    statsObject["node_type"] = NodeType::getNodeTypeName(matchingNode->getType()).toLower().replace(' ', '-');

                    QJsonDocument statsDocument(statsObject);

                    // send the response
                    connection->respond(HTTPConnection::StatusCode200, statsDocument.toJson(), qPrintable(JSON_MIME_TYPE));

                    // tell the caller we processed the request
                    return true;
                }

                return false;
            }
        }
    } else if (connection->requestOperation() == QNetworkAccessManager::PostOperation) {
        if (url.path() == URI_ASSIGNMENT) {
            // this is a script upload - ask the HTTPConnection to parse the form data
            QList<FormData> formData = connection->parseFormData();

            // check optional headers for # of instances and pool
            const QString ASSIGNMENT_INSTANCES_HEADER = "ASSIGNMENT-INSTANCES";
            const QString ASSIGNMENT_POOL_HEADER = "ASSIGNMENT-POOL";

            QByteArray assignmentInstancesValue = connection->requestHeader(ASSIGNMENT_INSTANCES_HEADER.toLocal8Bit());

            int numInstances = 1;

            if (!assignmentInstancesValue.isEmpty()) {
                // the user has requested a specific number of instances
                // so set that on the created assignment

                numInstances = assignmentInstancesValue.toInt();
            }

            QString assignmentPool = emptyPool;
            QByteArray assignmentPoolValue = connection->requestHeader(ASSIGNMENT_POOL_HEADER.toLocal8Bit());

            if (!assignmentPoolValue.isEmpty()) {
                // specific pool requested, set that on the created assignment
                assignmentPool = QString(assignmentPoolValue);
            }


            for (int i = 0; i < numInstances; i++) {
                // create an assignment for this saved script
                Assignment* scriptAssignment = new Assignment(Assignment::CreateCommand, Assignment::AgentType, assignmentPool);

                _ephemeralACScripts[scriptAssignment->getUUID()] = formData[0].second;

                // add the script assignment to the assignment queue
                SharedAssignmentPointer sharedScriptedAssignment(scriptAssignment);
                _unfulfilledAssignments.enqueue(sharedScriptedAssignment);
                _allAssignments.insert(sharedScriptedAssignment->getUUID(), sharedScriptedAssignment);
            }

            // respond with a 200 code for successful upload
            connection->respond(HTTPConnection::StatusCode200);

            return true;
        } else if (url.path() == URI_CONTENT_UPLOAD) {
            // this is an entity file upload, ask the HTTPConnection to parse the data
            QList<FormData> formData = connection->parseFormData();

            if (formData.size() > 0 && formData[0].second.size() > 0) {
                auto& firstFormData = formData[0];

                // check the file extension to see what kind of file this is
                // to make sure we handle this filetype for a content restore
                auto dispositionValue = QString(firstFormData.first.value("Content-Disposition"));
                auto formDataFilenameRegex = QRegExp("filename=\"(.+)\"");
                auto matchIndex = formDataFilenameRegex.indexIn(dispositionValue);

                QString uploadedFilename = "";
                if (matchIndex != -1) {
                    uploadedFilename = formDataFilenameRegex.cap(1);
                }

                if (uploadedFilename.endsWith(".json", Qt::CaseInsensitive)
                    || uploadedFilename.endsWith(".json.gz", Qt::CaseInsensitive)) {
                    // invoke our method to hand the new octree file off to the octree server
                    QMetaObject::invokeMethod(this, "handleOctreeFileReplacement",
                                              Qt::QueuedConnection, Q_ARG(QByteArray, firstFormData.second));

                    // respond with a 200 for success
                    connection->respond(HTTPConnection::StatusCode200);
                } else if (uploadedFilename.endsWith(".zip", Qt::CaseInsensitive)) {
                    auto deferred = makePromise("recoverFromUploadedBackup");

                    deferred->then([connectionPtr, JSON_MIME_TYPE](QString error, QVariantMap result) {
                        if (!connectionPtr) {
                            return;
                        }

                        QJsonObject rootJSON;
                        auto success = result["success"].toBool();
                        rootJSON["success"] = success;
                        QJsonDocument docJSON(rootJSON);
                        connectionPtr->respond(success ? HTTPConnection::StatusCode200 : HTTPConnection::StatusCode400, docJSON.toJson(),
                                            JSON_MIME_TYPE.toUtf8());
                    });

                    _contentManager->recoverFromUploadedBackup(deferred, firstFormData.second);

                    return true;
                } else {
                    // we don't have handling for this filetype, send back a 400 for failure
                    connection->respond(HTTPConnection::StatusCode400);
                }

            } else {
                // respond with a 400 for failure
                connection->respond(HTTPConnection::StatusCode400);
            }

            return true;

        } else if (url.path() == URI_API_BACKUPS) {
            auto params = connection->parseUrlEncodedForm();
            auto it = params.find("name");
            if (it == params.end()) {
                connection->respond(HTTPConnection::StatusCode400, "Bad request, missing `name`");
                return true;
            }

            auto deferred = makePromise("createManualBackup");
            deferred->then([connectionPtr, JSON_MIME_TYPE](QString error, QVariantMap result) {
                if (!connectionPtr) {
                    return;
                }

                QJsonObject rootJSON;
                auto success = result["success"].toBool();
                rootJSON["success"] = success;
                QJsonDocument docJSON(rootJSON);
                connectionPtr->respond(success ? HTTPConnection::StatusCode200 : HTTPConnection::StatusCode400, docJSON.toJson(),
                                    JSON_MIME_TYPE.toUtf8());
            });
            _contentManager->createManualBackup(deferred, it.value());

            return true;

        } else if (url.path() == "/domain_settings") {
            auto accessTokenVariant = _settingsManager.valueForKeyPath(ACCESS_TOKEN_KEY_PATH);
            if (!accessTokenVariant.isValid()) {
                connection->respond(HTTPConnection::StatusCode400);
                return true;
            }

        } else if (url.path() == URI_API_DOMAINS) {
            return forwardMetaverseAPIRequest(connection, "/api/v1/domains", "domain", { "label" });

        } else if (url.path().startsWith(URI_API_BACKUPS_RECOVER)) {
            auto id = url.path().mid(QString(URI_API_BACKUPS_RECOVER).length());
            auto deferred = makePromise("recoverFromBackup");
            deferred->then([connectionPtr, JSON_MIME_TYPE](QString error, QVariantMap result) {
                if (!connectionPtr) {
                    return;
                }

                QJsonObject rootJSON;
                auto success = result["success"].toBool();
                rootJSON["success"] = success;
                QJsonDocument docJSON(rootJSON);
                connectionPtr->respond(success ? HTTPConnection::StatusCode200 : HTTPConnection::StatusCode400, docJSON.toJson(),
                                    JSON_MIME_TYPE.toUtf8());
            });
            _contentManager->recoverFromBackup(deferred, id);
            return true;
        }
    } else if (connection->requestOperation() == QNetworkAccessManager::PutOperation) {
        if (url.path() == URI_API_DOMAINS) {
            QVariant domainSetting;
            if (!getSetting(METAVERSE_DOMAIN_ID_KEY_PATH, domainSetting)) {
                connection->respond(HTTPConnection::StatusCode400, "Domain id has not been set");
                return true;
            }
            auto domainID = domainSetting.toString();
            return forwardMetaverseAPIRequest(connection, "/api/v1/domains/" + domainID, "domain",
                                              { }, { "network_address", "network_port", "label" });
        }  else if (url.path() == URI_API_PLACES) {
            auto accessTokenVariant = _settingsManager.valueForKeyPath(ACCESS_TOKEN_KEY_PATH);
            if (!accessTokenVariant.isValid()) {
                connection->respond(HTTPConnection::StatusCode400, "User access token has not been set");
                return true;
            }

            auto params = connection->parseUrlEncodedForm();

            auto it = params.find("place_id");
            if (it == params.end()) {
                connection->respond(HTTPConnection::StatusCode400);
                return true;
            }
            QString place_id = it.value();

            it = params.find("path");
            if (it == params.end()) {
                connection->respond(HTTPConnection::StatusCode400);
                return true;
            }
            QString path = it.value();

            it = params.find("domain_id");
            QString domainID;
            if (it == params.end()) {
                QVariant domainSetting;
                if (!getSetting(METAVERSE_DOMAIN_ID_KEY_PATH, domainSetting)) {
                    connection->respond(HTTPConnection::StatusCode400);
                    return true;
                }
                domainID = domainSetting.toString();
            } else {
                domainID = it.value();
            }

            QJsonObject root {
                {
                    "place",
                    QJsonObject({
                        { "pointee_query", domainID },
                        { "path", path }
                     })
                }
            };
            QJsonDocument doc(root);


            QUrl url { NetworkingConstants::METAVERSE_SERVER_URL().toString() + "/api/v1/places/" + place_id };

            url.setQuery("access_token=" + accessTokenVariant.toString());

            QNetworkRequest req(url);
            req.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            QNetworkReply* reply = NetworkAccessManager::getInstance().put(req, doc.toJson());

            connect(reply, &QNetworkReply::finished, this, [reply, connection]() {
                if (reply->error() != QNetworkReply::NoError) {
                    qDebug() << "Got error response from metaverse server: " << reply->readAll();
                    connection->respond(HTTPConnection::StatusCode500,
                                        "Error communicating with Metaverse");
                    return;
                }

                connection->respond(HTTPConnection::StatusCode200, reply->readAll());
            });
            return true;
        }

    } else if (connection->requestOperation() == QNetworkAccessManager::DeleteOperation) {
        const QString ALL_NODE_DELETE_REGEX_STRING = QString("\\%1\\/?$").arg(URI_NODES);
        const QString NODE_DELETE_REGEX_STRING = QString("\\%1\\/(%2)\\/$").arg(URI_NODES).arg(UUID_REGEX_STRING);

        QRegExp allNodesDeleteRegex(ALL_NODE_DELETE_REGEX_STRING);
        QRegExp nodeDeleteRegex(NODE_DELETE_REGEX_STRING);

        if (url.path().startsWith(URI_API_BACKUPS_ID)) {
            auto id = url.path().mid(QString(URI_API_BACKUPS_ID).length());
            auto deferred = makePromise("deleteBackup");
            deferred->then([connectionPtr, JSON_MIME_TYPE](QString error, QVariantMap result) {
                if (!connectionPtr) {
                    return;
                }

                QJsonObject rootJSON;
                auto success = result["success"].toBool();
                rootJSON["success"] = success;
                QJsonDocument docJSON(rootJSON);
                connectionPtr->respond(success ? HTTPConnection::StatusCode200 : HTTPConnection::StatusCode400, docJSON.toJson(),
                                    JSON_MIME_TYPE.toUtf8());
            });
            _contentManager->deleteBackup(deferred, id);

            return true;

        } else if (nodeDeleteRegex.indexIn(url.path()) != -1) {
            // this is a request to DELETE one node by UUID

            // pull the captured string, if it exists
            QUuid deleteUUID = QUuid(nodeDeleteRegex.cap(1));

            SharedNodePointer nodeToKill = nodeList->nodeWithUUID(deleteUUID);

            if (nodeToKill) {
                // start with a 200 response
                connection->respond(HTTPConnection::StatusCode200);

                // we have a valid UUID and node - kill the node that has this assignment
                QMetaObject::invokeMethod(nodeList.data(), "killNodeWithUUID", Q_ARG(const QUuid&, deleteUUID));

                // successfully processed request
                return true;
            }

            return true;
        } else if (allNodesDeleteRegex.indexIn(url.path()) != -1) {
            qDebug() << "Received request to kill all nodes.";
            nodeList->eraseAllNodes();

            return true;
        }
    }

    // didn't process the request, let our DomainServerSettingsManager or HTTPManager handle
    return _settingsManager.handleAuthenticatedHTTPRequest(connection, url);
}

static const QString HIFI_SESSION_COOKIE_KEY = "DS_WEB_SESSION_UUID";
static const QString STATE_QUERY_KEY = "state";

bool DomainServer::handleHTTPSRequest(HTTPSConnection* connection, const QUrl &url, bool skipSubHandler) {
    if (url.path() == URI_OAUTH) {

        QUrlQuery codeURLQuery(url);

        const QString CODE_QUERY_KEY = "code";
        QString authorizationCode = codeURLQuery.queryItemValue(CODE_QUERY_KEY);

        QUuid stateUUID = QUuid(codeURLQuery.queryItemValue(STATE_QUERY_KEY));

        if (!authorizationCode.isEmpty() && !stateUUID.isNull() && _webAuthenticationStateSet.remove(stateUUID)) {
            // fire off a request with this code and state to get an access token for the user

            const QString OAUTH_TOKEN_REQUEST_PATH = "/oauth/token";
            QUrl tokenRequestUrl = _oauthProviderURL;
            tokenRequestUrl.setPath(OAUTH_TOKEN_REQUEST_PATH);

            const QString OAUTH_GRANT_TYPE_POST_STRING = "grant_type=authorization_code";
            QString tokenPostBody = OAUTH_GRANT_TYPE_POST_STRING;
            tokenPostBody += QString("&code=%1&redirect_uri=%2&client_id=%3&client_secret=%4")
                .arg(authorizationCode, oauthRedirectURL().toString(), _oauthClientID, _oauthClientSecret);

            QNetworkRequest tokenRequest(tokenRequestUrl);
            tokenRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
            tokenRequest.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);
            tokenRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

            QNetworkReply* tokenReply = NetworkAccessManager::getInstance().post(tokenRequest, tokenPostBody.toLocal8Bit());
            connect(tokenReply, &QNetworkReply::finished, this, &DomainServer::tokenGrantFinished);

            // add this connection to our list of pending connections so that we can hold the response
            _pendingOAuthConnections.insert(stateUUID, connection);

            // set the state UUID on the reply so that we can associate the response with the connection later
            tokenReply->setProperty(STATE_QUERY_KEY.toLocal8Bit(), stateUUID);

            return true;
        } else {
            connection->respond(HTTPConnection::StatusCode400);

            return true;
        }
    } else {
        return false;
    }
}

HTTPSConnection* DomainServer::connectionFromReplyWithState(QNetworkReply* reply) {
    // grab the UUID state property from the reply
    QUuid stateUUID = reply->property(STATE_QUERY_KEY.toLocal8Bit()).toUuid();

    if (!stateUUID.isNull()) {
        return _pendingOAuthConnections.take(stateUUID);
    } else {
        return nullptr;
    }
}

void DomainServer::tokenGrantFinished() {
    auto tokenReply = qobject_cast<QNetworkReply*>(sender());

    if (tokenReply) {
        if (tokenReply->error() == QNetworkReply::NoError) {
            // now that we have a token for this profile, send off a profile request
            QNetworkReply* profileReply = profileRequestGivenTokenReply(tokenReply);

            // forward along the state UUID that we kept with the token request
            profileReply->setProperty(STATE_QUERY_KEY.toLocal8Bit(), tokenReply->property(STATE_QUERY_KEY.toLocal8Bit()));

            connect(profileReply, &QNetworkReply::finished, this, &DomainServer::profileRequestFinished);
        } else {
            // the token grant failed, send back a 500 (assuming the connection is still around)
            auto connection = connectionFromReplyWithState(tokenReply);

            if (connection) {
                connection->respond(HTTPConnection::StatusCode500);
            }
        }

        tokenReply->deleteLater();
    }
}

void DomainServer::profileRequestFinished() {

    auto profileReply = qobject_cast<QNetworkReply*>(sender());

    if (profileReply) {
        auto connection = connectionFromReplyWithState(profileReply);

        if (connection) {
            if (profileReply->error() == QNetworkReply::NoError) {
                // call helper method to get cookieHeaders
                Headers cookieHeaders = setupCookieHeadersFromProfileReply(profileReply);

                connection->respond(HTTPConnection::StatusCode302, QByteArray(),
                                    HTTPConnection::DefaultContentType, cookieHeaders);

            } else {
                // the profile request failed, send back a 500 (assuming the connection is still around)
                connection->respond(HTTPConnection::StatusCode500);
            }
        }

        profileReply->deleteLater();
    }
}

bool DomainServer::isAuthenticatedRequest(HTTPConnection* connection, const QUrl& url) {

    const QByteArray HTTP_COOKIE_HEADER_KEY = "Cookie";
    const QString ADMIN_USERS_CONFIG_KEY = "admin-users";
    const QString ADMIN_ROLES_CONFIG_KEY = "admin-roles";
    const QString BASIC_AUTH_USERNAME_KEY_PATH = "security.http_username";
    const QString BASIC_AUTH_PASSWORD_KEY_PATH = "security.http_password";

    const QByteArray UNAUTHENTICATED_BODY = "You do not have permission to access this domain-server.";

    QVariant adminUsersVariant = _settingsManager.valueForKeyPath(ADMIN_USERS_CONFIG_KEY);
    QVariant adminRolesVariant = _settingsManager.valueForKeyPath(ADMIN_ROLES_CONFIG_KEY);

    if (!_oauthProviderURL.isEmpty()
        && (adminUsersVariant.isValid() || adminRolesVariant.isValid())) {
        QString cookieString = connection->requestHeader(HTTP_COOKIE_HEADER_KEY);

        const QString COOKIE_UUID_REGEX_STRING = HIFI_SESSION_COOKIE_KEY + "=([\\d\\w-]+)($|;)";
        QRegExp cookieUUIDRegex(COOKIE_UUID_REGEX_STRING);

        QUuid cookieUUID;
        if (cookieString.indexOf(cookieUUIDRegex) != -1) {
            cookieUUID = cookieUUIDRegex.cap(1);
        }

        if (_settingsManager.valueForKeyPath(BASIC_AUTH_USERNAME_KEY_PATH).isValid()) {
            qDebug() << "Config file contains web admin settings for OAuth and basic HTTP authentication."
                << "These cannot be combined - using OAuth for authentication.";
        }

        if (!cookieUUID.isNull() && _cookieSessionHash.contains(cookieUUID)) {
            // pull the QJSONObject for the user with this cookie UUID
            DomainServerWebSessionData sessionData = _cookieSessionHash.value(cookieUUID);
            QString profileUsername = sessionData.getUsername();

            if (_settingsManager.valueForKeyPath(ADMIN_USERS_CONFIG_KEY).toStringList().contains(profileUsername)) {
                // this is an authenticated user
                return true;
            }

            // loop the roles of this user and see if they are in the admin-roles array
            QStringList adminRolesArray = _settingsManager.valueForKeyPath(ADMIN_ROLES_CONFIG_KEY).toStringList();

            if (!adminRolesArray.isEmpty()) {
                foreach(const QString& userRole, sessionData.getRoles()) {
                    if (adminRolesArray.contains(userRole)) {
                        // this user has a role that allows them to administer the domain-server
                        return true;
                    }
                }
            }

            connection->respond(HTTPConnection::StatusCode401, UNAUTHENTICATED_BODY);

            // the user does not have allowed username or role, return 401
            return false;
        } else {
            static const QByteArray REQUESTED_WITH_HEADER = "X-Requested-With";
            static const QString XML_REQUESTED_WITH = "XMLHttpRequest";

            if (connection->requestHeader(REQUESTED_WITH_HEADER) == XML_REQUESTED_WITH) {
                // unauthorized XHR requests get a 401 and not a 302, since there isn't an XHR
                // path to OAuth authorize
                connection->respond(HTTPConnection::StatusCode401, UNAUTHENTICATED_BODY);
            } else {
                // re-direct this user to OAuth page

                // generate a random state UUID to use
                QUuid stateUUID = QUuid::createUuid();

                // add it to the set so we can handle the callback from the OAuth provider
                _webAuthenticationStateSet.insert(stateUUID);

                QUrl authURL = oauthAuthorizationURL(stateUUID);

                Headers redirectHeaders;

                redirectHeaders.insert("Location", authURL.toEncoded());

                connection->respond(HTTPConnection::StatusCode302,
                                    QByteArray(), HTTPConnection::DefaultContentType, redirectHeaders);
            }

            // we don't know about this user yet, so they are not yet authenticated
            return false;
        }
    } else if (_settingsManager.valueForKeyPath(BASIC_AUTH_USERNAME_KEY_PATH).isValid()) {
        // config file contains username and password combinations for basic auth
        const QByteArray BASIC_AUTH_HEADER_KEY = "Authorization";

        // check if a username and password have been provided with the request
        QString basicAuthString = connection->requestHeader(BASIC_AUTH_HEADER_KEY);

        if (!basicAuthString.isEmpty()) {
            QStringList splitAuthString = basicAuthString.split(' ');
            QString base64String = splitAuthString.size() == 2 ? splitAuthString[1] : "";
            QString credentialString = QByteArray::fromBase64(base64String.toLocal8Bit());

            if (!credentialString.isEmpty()) {
                QStringList credentialList = credentialString.split(':');
                if (credentialList.size() == 2) {
                    QString headerUsername = credentialList[0];
                    QString headerPassword = credentialList[1];

                    // we've pulled a username and password - now check if there is a match in our basic auth hash
                    QString settingsUsername = _settingsManager.valueForKeyPath(BASIC_AUTH_USERNAME_KEY_PATH).toString();
                    QVariant settingsPasswordVariant = _settingsManager.valueForKeyPath(BASIC_AUTH_PASSWORD_KEY_PATH);

                    QString settingsPassword = settingsPasswordVariant.isValid() ? settingsPasswordVariant.toString() : "";
                    QString hexHeaderPassword = headerPassword.isEmpty() ?
                        "" : QCryptographicHash::hash(headerPassword.toUtf8(), QCryptographicHash::Sha256).toHex();
                        
                    if (settingsUsername == headerUsername && hexHeaderPassword == settingsPassword) {
                        return true;
                    }
                }
            }
        }

        // basic HTTP auth being used but no username and password are present
        // or the username and password are not correct
        // send back a 401 and ask for basic auth

        const QByteArray HTTP_AUTH_REQUEST_HEADER_KEY = "WWW-Authenticate";
        static QString HTTP_AUTH_REALM_STRING = QString("Basic realm='%1 %2'")
            .arg(_hostname.isEmpty() ? "localhost" : _hostname)
            .arg("domain-server");

        Headers basicAuthHeader;
        basicAuthHeader.insert(HTTP_AUTH_REQUEST_HEADER_KEY, HTTP_AUTH_REALM_STRING.toUtf8());

        connection->respond(HTTPConnection::StatusCode401, UNAUTHENTICATED_BODY,
                            HTTPConnection::DefaultContentType, basicAuthHeader);

        // not authenticated, bubble up false
        return false;

    } else {
        // we don't have an OAuth URL + admin roles/usernames, so all users are authenticated
        return true;
    }
}

const QString OAUTH_JSON_ACCESS_TOKEN_KEY = "access_token";
QNetworkReply* DomainServer::profileRequestGivenTokenReply(QNetworkReply* tokenReply) {
    // pull the access token from the returned JSON and store it with the matching session UUID
    QJsonDocument returnedJSON = QJsonDocument::fromJson(tokenReply->readAll());
    QString accessToken = returnedJSON.object()[OAUTH_JSON_ACCESS_TOKEN_KEY].toString();

    // fire off a request to get this user's identity so we can see if we will let them in
    QUrl profileURL = _oauthProviderURL;
    profileURL.setPath("/api/v1/user/profile");
    profileURL.setQuery(QString("%1=%2").arg(OAUTH_JSON_ACCESS_TOKEN_KEY, accessToken));

    qDebug() << "Sending profile request to: " << profileURL;

    QNetworkRequest profileRequest(profileURL);
    profileRequest.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    profileRequest.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);
    return NetworkAccessManager::getInstance().get(profileRequest);
}

Headers DomainServer::setupCookieHeadersFromProfileReply(QNetworkReply* profileReply) {
    Headers cookieHeaders;

    // create a UUID for this cookie
    QUuid cookieUUID = QUuid::createUuid();

    QJsonDocument profileDocument = QJsonDocument::fromJson(profileReply->readAll());
    QJsonObject userObject = profileDocument.object()["data"].toObject()["user"].toObject();

    // add the profile to our in-memory data structure so we know who the user is when they send us their cookie
    DomainServerWebSessionData sessionData(userObject);
    _cookieSessionHash.insert(cookieUUID, sessionData);

    // setup expiry for cookie to 1 month from today
    QDateTime cookieExpiry = QDateTime::currentDateTimeUtc().addMonths(1);

    QString cookieString = HIFI_SESSION_COOKIE_KEY + "=" + uuidStringWithoutCurlyBraces(cookieUUID.toString());
    cookieString += "; expires=" + cookieExpiry.toString("ddd, dd MMM yyyy HH:mm:ss") + " GMT";
    cookieString += "; domain=" + _hostname + "; path=/";

    cookieHeaders.insert("Set-Cookie", cookieString.toUtf8());

    // redirect the user back to the homepage so they can present their cookie and be authenticated
    cookieHeaders.insert("Location", pathForRedirect().toUtf8());

    return cookieHeaders;
}

void DomainServer::refreshStaticAssignmentAndAddToQueue(SharedAssignmentPointer& assignment) {
    QUuid oldUUID = assignment->getUUID();
    assignment->resetUUID();

    qDebug() << "Reset UUID for assignment -" << *assignment.data() << "- and added to queue. Old UUID was"
        << uuidStringWithoutCurlyBraces(oldUUID);

    if (assignment->getType() == Assignment::AgentType && assignment->getPayload().isEmpty()) {
        // if this was an Agent without a script URL, we need to rename the old file so it can be retrieved at the new UUID
        QFile::rename(pathForAssignmentScript(oldUUID), pathForAssignmentScript(assignment->getUUID()));
    }

    // add the static assignment back under the right UUID, and to the queue
    _allAssignments.insert(assignment->getUUID(), assignment);
    _unfulfilledAssignments.enqueue(assignment);
}

static const QString BROADCASTING_SETTINGS_KEY = "broadcasting";

struct ReplicationServerInfo {
    NodeType_t nodeType;
    HifiSockAddr sockAddr;
};

ReplicationServerInfo serverInformationFromSettings(QVariantMap serverMap, ReplicationServerDirection direction) {
    static const QString REPLICATION_SERVER_ADDRESS = "address";
    static const QString REPLICATION_SERVER_PORT = "port";
    static const QString REPLICATION_SERVER_TYPE = "server_type";

    if (serverMap.contains(REPLICATION_SERVER_ADDRESS) && serverMap.contains(REPLICATION_SERVER_PORT)
        && serverMap.contains(REPLICATION_SERVER_TYPE)) {

        auto nodeType = NodeType::fromString(serverMap[REPLICATION_SERVER_TYPE].toString());

        ReplicationServerInfo serverInfo;

        if (direction == Upstream) {
            serverInfo.nodeType = NodeType::upstreamType(nodeType);
        } else if (direction == Downstream) {
            serverInfo.nodeType = NodeType::downstreamType(nodeType);
        }

        // read the address and port and construct a HifiSockAddr from them
        serverInfo.sockAddr = {
            serverMap[REPLICATION_SERVER_ADDRESS].toString(),
            (quint16) serverMap[REPLICATION_SERVER_PORT].toString().toInt()
        };

        return serverInfo;
    }

    return { NodeType::Unassigned, HifiSockAddr() };
}

void DomainServer::updateReplicationNodes(ReplicationServerDirection direction) {

    auto broadcastSettingsVariant = _settingsManager.valueForKeyPath(BROADCASTING_SETTINGS_KEY);

    if (broadcastSettingsVariant.isValid()) {
        auto nodeList = DependencyManager::get<LimitedNodeList>();
        std::vector<HifiSockAddr> replicationNodesInSettings;

        auto replicationSettings = broadcastSettingsVariant.toMap();

        QString serversKey = direction == Upstream ? "upstream_servers" : "downstream_servers";
        QString replicationDirection = direction == Upstream ? "upstream" : "downstream";

        if (replicationSettings.contains(serversKey)) {
            auto serversSettings = replicationSettings.value(serversKey).toList();

            std::vector<HifiSockAddr> knownReplicationNodes;
            nodeList->eachNode([&](const SharedNodePointer& otherNode) {
                if ((direction == Upstream && NodeType::isUpstream(otherNode->getType()))
                    || (direction == Downstream && NodeType::isDownstream(otherNode->getType()))) {
                    knownReplicationNodes.push_back(otherNode->getPublicSocket());
                }
            });

            for (auto& server : serversSettings) {
                auto replicationServer = serverInformationFromSettings(server.toMap(), direction);

                if (!replicationServer.sockAddr.isNull() && replicationServer.nodeType != NodeType::Unassigned) {
                    // make sure we have the settings we need for this replication server
                    replicationNodesInSettings.push_back(replicationServer.sockAddr);

                    bool knownNode = find(knownReplicationNodes.cbegin(), knownReplicationNodes.cend(),
                                          replicationServer.sockAddr) != knownReplicationNodes.cend();
                    if (!knownNode) {
                        // manually add the replication node to our node list
                        auto node = nodeList->addOrUpdateNode(QUuid::createUuid(), replicationServer.nodeType,
                                                              replicationServer.sockAddr, replicationServer.sockAddr,
                                                              Node::NULL_LOCAL_ID, false, direction == Upstream);
                        node->setIsForcedNeverSilent(true);

                        qDebug() << "Adding" << (direction == Upstream ? "upstream" : "downstream")
                            << "node:" << node->getUUID() << replicationServer.sockAddr;

                        // manually activate the public socket for the replication node
                        node->activatePublicSocket();
                    }
                }

            }
        }

        // enumerate the nodes to determine which are no longer downstream for this domain
        // collect them in a vector to separately remove them with handleKillNode (since eachNode has a read lock and
        // we cannot recursively take the write lock required by handleKillNode)
        std::vector<SharedNodePointer> nodesToKill;
        nodeList->eachNode([&](const SharedNodePointer& otherNode) {
            if ((direction == Upstream && NodeType::isUpstream(otherNode->getType()))
                || (direction == Downstream && NodeType::isDownstream(otherNode->getType()))) {
                bool nodeInSettings = find(replicationNodesInSettings.cbegin(), replicationNodesInSettings.cend(),
                                           otherNode->getPublicSocket()) != replicationNodesInSettings.cend();
                if (!nodeInSettings) {
                    qDebug() << "Removing" << replicationDirection
                        << "node:" << otherNode->getUUID() << otherNode->getPublicSocket();
                    nodesToKill.push_back(otherNode);
                }
            }
        });

        for (auto& node : nodesToKill) {
            handleKillNode(node);
        }
    }
}

void DomainServer::updateDownstreamNodes() {
    updateReplicationNodes(Downstream);
}

void DomainServer::updateUpstreamNodes() {
    updateReplicationNodes(Upstream);
}

void DomainServer::updateReplicatedNodes() {
    // Make sure we have downstream nodes in our list
    static const QString REPLICATED_USERS_KEY = "users";
    _replicatedUsernames.clear();

    auto replicationVariant = _settingsManager.valueForKeyPath(BROADCASTING_SETTINGS_KEY);
    if (replicationVariant.isValid()) {
        auto replicationSettings = replicationVariant.toMap();
        if (replicationSettings.contains(REPLICATED_USERS_KEY)) {
            auto usersSettings = replicationSettings.value(REPLICATED_USERS_KEY).toList();
            for (auto& username : usersSettings) {
                _replicatedUsernames.push_back(username.toString().toLower());
            }
        }
    }

    auto nodeList = DependencyManager::get<LimitedNodeList>();
    nodeList->eachMatchingNode([](const SharedNodePointer& otherNode) -> bool {
            return otherNode->getType() == NodeType::Agent;
        }, [this](const SharedNodePointer& otherNode) {
            auto shouldReplicate = shouldReplicateNode(*otherNode);
            auto isReplicated = otherNode->isReplicated();
            if (isReplicated && !shouldReplicate) {
                qDebug() << "Setting node to NOT be replicated:"
                    << otherNode->getPermissions().getVerifiedUserName() << otherNode->getUUID();
            } else if (!isReplicated && shouldReplicate) {
                qDebug() << "Setting node to replicated:"
                    << otherNode->getPermissions().getVerifiedUserName() << otherNode->getUUID();
            }
            otherNode->setIsReplicated(shouldReplicate);
        }
    );
}

bool DomainServer::shouldReplicateNode(const Node& node) {
    if (node.getType() == NodeType::Agent) {
        QString verifiedUsername = node.getPermissions().getVerifiedUserName();

        // Both the verified username and usernames in _replicatedUsernames are lowercase, so
        // comparisons here are case-insensitive.
        auto it = find(_replicatedUsernames.cbegin(), _replicatedUsernames.cend(), verifiedUsername);
        return it != _replicatedUsernames.end();
    } else {
        return false;
    }
};

void DomainServer::nodeAdded(SharedNodePointer node) {
    // we don't use updateNodeWithData, so add the DomainServerNodeData to the node here
    node->setLinkedData(std::unique_ptr<DomainServerNodeData> { new DomainServerNodeData() });
}

void DomainServer::nodeKilled(SharedNodePointer node) {
    // if this peer connected via ICE then remove them from our ICE peers hash
    _gatekeeper.removeICEPeer(node->getUUID());

    DomainServerNodeData* nodeData = static_cast<DomainServerNodeData*>(node->getLinkedData());

    if (nodeData) {
        // if this node's UUID matches a static assignment we need to throw it back in the assignment queue
        if (!nodeData->getAssignmentUUID().isNull()) {
            SharedAssignmentPointer matchedAssignment = _allAssignments.take(nodeData->getAssignmentUUID());

            if (matchedAssignment && matchedAssignment->isStatic()) {
                refreshStaticAssignmentAndAddToQueue(matchedAssignment);
            }
        }

        // cleanup the connection secrets that we set up for this node (on the other nodes)
        foreach (const QUuid& otherNodeSessionUUID, nodeData->getSessionSecretHash().keys()) {
            SharedNodePointer otherNode = DependencyManager::get<LimitedNodeList>()->nodeWithUUID(otherNodeSessionUUID);
            if (otherNode) {
                static_cast<DomainServerNodeData*>(otherNode->getLinkedData())->getSessionSecretHash().remove(node->getUUID());
            }
        }

        if (node->getType() == NodeType::Agent) {
            // if this node was an Agent ask DomainServerNodeData to remove the interpolation we potentially stored
            nodeData->removeOverrideForKey(USERNAME_UUID_REPLACEMENT_STATS_KEY,
                    uuidStringWithoutCurlyBraces(node->getUUID()));

            // if this node is a user (unassigned Agent), signal
            if (!nodeData->wasAssigned()) {
                emit userDisconnected();
            }
        }
    }
}

SharedAssignmentPointer DomainServer::dequeueMatchingAssignment(const QUuid& assignmentUUID, NodeType_t nodeType) {
    QQueue<SharedAssignmentPointer>::iterator i = _unfulfilledAssignments.begin();

    while (i != _unfulfilledAssignments.end()) {
        if (i->data()->getType() == Assignment::typeForNodeType(nodeType)
            && i->data()->getUUID() == assignmentUUID) {
            // we have an unfulfilled assignment to return

            // return the matching assignment
            return _unfulfilledAssignments.takeAt(i - _unfulfilledAssignments.begin());
        } else {
            ++i;
        }
    }

    return SharedAssignmentPointer();
}

SharedAssignmentPointer DomainServer::deployableAssignmentForRequest(const Assignment& requestAssignment) {
    // this is an unassigned client talking to us directly for an assignment
    // go through our queue and see if there are any assignments to give out
    QQueue<SharedAssignmentPointer>::iterator sharedAssignment = _unfulfilledAssignments.begin();

    while (sharedAssignment != _unfulfilledAssignments.end()) {
        Assignment* assignment = sharedAssignment->data();
        bool requestIsAllTypes = requestAssignment.getType() == Assignment::AllTypes;
        bool assignmentTypesMatch = assignment->getType() == requestAssignment.getType();
        bool neitherHasPool = assignment->getPool().isEmpty() && requestAssignment.getPool().isEmpty();
        bool assignmentPoolsMatch = assignment->getPool() == requestAssignment.getPool();

        if ((requestIsAllTypes || assignmentTypesMatch) && (neitherHasPool || assignmentPoolsMatch)) {

            // remove the assignment from the queue
            SharedAssignmentPointer deployableAssignment = _unfulfilledAssignments.takeAt(sharedAssignment
                                                                                          - _unfulfilledAssignments.begin());

            // until we get a connection for this assignment
            // put assignment back in queue but stick it at the back so the others have a chance to go out
            _unfulfilledAssignments.enqueue(deployableAssignment);

            // stop looping, we've handed out an assignment
            return deployableAssignment;
        } else {
            // push forward the iterator to check the next assignment
            ++sharedAssignment;
        }
    }

    return SharedAssignmentPointer();
}

void DomainServer::addStaticAssignmentsToQueue() {

    // if the domain-server has just restarted,
    // check if there are static assignments that we need to throw into the assignment queue
    auto sharedAssignments = _allAssignments.values();

    // sort the assignments to put the server/mixer assignments first
    qSort(sharedAssignments.begin(), sharedAssignments.end(), [](SharedAssignmentPointer a, SharedAssignmentPointer b){
        if (a->getType() == b->getType()) {
            return true;
        } else if (a->getType() != Assignment::AgentType && b->getType() != Assignment::AgentType) {
            return a->getType() < b->getType();
        } else {
            return a->getType() != Assignment::AgentType;
        }
    });

    auto staticAssignment = sharedAssignments.begin();

    while (staticAssignment != sharedAssignments.end()) {
        // add any of the un-matched static assignments to the queue

        // enumerate the nodes and check if there is one with an attached assignment with matching UUID
        if (!DependencyManager::get<LimitedNodeList>()->nodeWithUUID((*staticAssignment)->getUUID())) {
            // this assignment has not been fulfilled - reset the UUID and add it to the assignment queue
            refreshStaticAssignmentAndAddToQueue(*staticAssignment);
        }

        ++staticAssignment;
    }
}

void DomainServer::processPathQueryPacket(QSharedPointer<ReceivedMessage> message) {
    // this is a query for the viewpoint resulting from a path
    // first pull the query path from the packet

    // figure out how many bytes the sender said this path is
    quint16 numPathBytes;
    message->readPrimitive(&numPathBytes);

    if (numPathBytes <= message->getBytesLeftToRead()) {
        // the number of path bytes makes sense for the sent packet - pull out the path
        QString pathQuery = QString::fromUtf8(message->getRawMessage() + message->getPosition(), numPathBytes);

        // our settings contain paths that start with a leading slash, so make sure this query has that
        if (!pathQuery.startsWith("/")) {
            pathQuery.prepend("/");
        }

        const QString PATHS_SETTINGS_KEYPATH_FORMAT = "%1.%2";
        const QString PATH_VIEWPOINT_KEY = "viewpoint";
        const QString INDEX_PATH = "/";

        // check out paths in the _configMap to see if we have a match
        auto keypath = QString(PATHS_SETTINGS_KEYPATH_FORMAT).arg(SETTINGS_PATHS_KEY).arg(pathQuery);
        QVariant pathMatch = _settingsManager.valueForKeyPath(keypath);

        if (pathMatch.isValid() || pathQuery == INDEX_PATH) {
            // we got a match, respond with the resulting viewpoint
            auto nodeList = DependencyManager::get<LimitedNodeList>();

            QString responseViewpoint;

            // if we didn't match the path BUT this is for the index path then send back our default
            if (pathMatch.isValid()) {
                responseViewpoint = pathMatch.toMap()[PATH_VIEWPOINT_KEY].toString();
            } else {
                const QString DEFAULT_INDEX_PATH = "/0,0,0/0,0,0,1";
                responseViewpoint = DEFAULT_INDEX_PATH;
            }

            if (!responseViewpoint.isEmpty()) {
                QByteArray viewpointUTF8 = responseViewpoint.toUtf8();

                // prepare a packet for the response
                auto pathResponsePacket = NLPacket::create(PacketType::DomainServerPathResponse, -1, true);

                // check the number of bytes the viewpoint is
                quint16 numViewpointBytes = viewpointUTF8.size();

                // are we going to be able to fit this response viewpoint in a packet?
                if (numPathBytes + numViewpointBytes + sizeof(numViewpointBytes) + sizeof(numPathBytes)
                        < (unsigned long) pathResponsePacket->bytesAvailableForWrite()) {
                    // append the number of bytes this path is
                    pathResponsePacket->writePrimitive(numPathBytes);

                    // append the path itself
                    pathResponsePacket->write(pathQuery.toUtf8());

                    // append the number of bytes the resulting viewpoint is
                    pathResponsePacket->writePrimitive(numViewpointBytes);

                    // append the viewpoint itself
                    pathResponsePacket->write(viewpointUTF8);

                    qDebug() << "Sending a viewpoint response for path query" << pathQuery << "-" << viewpointUTF8;

                    // send off the packet - see if we can associate this outbound data to a particular node
                    // TODO: does this senderSockAddr always work for a punched DS client?
                    nodeList->sendPacket(std::move(pathResponsePacket), message->getSenderSockAddr());
                }
            }

        } else {
            // we don't respond if there is no match - this may need to change once this packet
            // query/response is made reliable
            qDebug() << "No match for path query" << pathQuery << "- refusing to respond.";
        }
    }
}

void DomainServer::processNodeDisconnectRequestPacket(QSharedPointer<ReceivedMessage> message) {
    // This packet has been matched to a source node and they're asking not to be in the domain anymore
    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();

    auto localID = message->getSourceID();
    qDebug() << "Received a disconnect request from node with local ID" << localID;

    // we want to check what type this node was before going to kill it so that we can avoid sending the RemovedNode
    // packet to nodes that don't care about this type
    auto nodeToKill = limitedNodeList->nodeWithLocalID(localID);

    if (nodeToKill) {
        handleKillNode(nodeToKill);
    }
}

void DomainServer::handleKillNode(SharedNodePointer nodeToKill) {
    auto limitedNodeList = DependencyManager::get<LimitedNodeList>();
    const QUuid& nodeUUID = nodeToKill->getUUID();

    limitedNodeList->killNodeWithUUID(nodeUUID);

    static auto removedNodePacket = NLPacket::create(PacketType::DomainServerRemovedNode, NUM_BYTES_RFC4122_UUID);

    removedNodePacket->reset();
    removedNodePacket->write(nodeUUID.toRfc4122());

    // broadcast out the DomainServerRemovedNode message
    limitedNodeList->eachMatchingNode([this, &nodeToKill](const SharedNodePointer& otherNode) -> bool {
        // only send the removed node packet to nodes that care about the type of node this was
        return isInInterestSet(otherNode, nodeToKill);
    }, [&limitedNodeList](const SharedNodePointer& otherNode){
        limitedNodeList->sendUnreliablePacket(*removedNodePacket, *otherNode);
    });
}

void DomainServer::processICEServerHeartbeatDenialPacket(QSharedPointer<ReceivedMessage> message) {
    static const int NUM_HEARTBEAT_DENIALS_FOR_KEYPAIR_REGEN = 3;

    if (++_numHeartbeatDenials > NUM_HEARTBEAT_DENIALS_FOR_KEYPAIR_REGEN) {
        qDebug() << "Received" << NUM_HEARTBEAT_DENIALS_FOR_KEYPAIR_REGEN << "heartbeat denials from ice-server"
            << "- re-generating keypair now";

        // we've hit our threshold of heartbeat denials, trigger a keypair re-generation
        auto limitedNodeList = DependencyManager::get<LimitedNodeList>();
        DependencyManager::get<AccountManager>()->generateNewDomainKeypair(limitedNodeList->getSessionUUID());

        // reset our number of heartbeat denials
        _numHeartbeatDenials = 0;
    }

    // even though we can't get into this ice-server it is responding to us, so we reset our number of no-reply heartbeats
    _noReplyICEHeartbeats = 0;
}

void DomainServer::processICEServerHeartbeatACK(QSharedPointer<ReceivedMessage> message) {
    // we don't do anything with this ACK other than use it to tell us to keep talking to the same ice-server
    _noReplyICEHeartbeats = 0;

    if (!_connectedToICEServer) {
        _connectedToICEServer = true;
        sendICEServerAddressToMetaverseAPI();
        qInfo() << "Connected to ice-server at" << _iceServerSocket;
    }
}

void DomainServer::handleKeypairChange() {
    if (_iceServerHeartbeatPacket) {
        // reset the payload size of the ice-server heartbeat packet - this causes the packet to be re-generated
        // the next time we go to send an ice-server heartbeat
        _iceServerHeartbeatPacket->setPayloadSize(0);

        // send a heartbeat to the ice server immediately
        sendHeartbeatToIceServer();
    }
}

void DomainServer::handleICEHostInfo(const QHostInfo& hostInfo) {
    // clear the ICE address lookup ID so that it can fire again
    _iceAddressLookupID = INVALID_ICE_LOOKUP_ID;

    // enumerate the returned addresses and collect only valid IPv4 addresses
    QList<QHostAddress> sanitizedAddresses = hostInfo.addresses();
    auto it = sanitizedAddresses.begin();
    while (it != sanitizedAddresses.end()) {
        if (!it->isNull() && it->protocol() == QAbstractSocket::IPv4Protocol) {
            ++it;
        } else {
            it = sanitizedAddresses.erase(it);
        }
    }

    if (hostInfo.error() != QHostInfo::NoError || sanitizedAddresses.empty()) {
        qWarning() << "IP address lookup failed for" << _iceServerAddr << ":" << hostInfo.errorString();

        // if we don't have an ICE server to use yet, trigger a retry
        if (_iceServerSocket.isNull()) {
            const int ICE_ADDRESS_LOOKUP_RETRY_MS = 1000;

            QTimer::singleShot(ICE_ADDRESS_LOOKUP_RETRY_MS, this, SLOT(updateICEServerAddresses()));
        }

    } else {
        int countBefore = _iceServerAddresses.count();

        _iceServerAddresses = sanitizedAddresses;

        if (countBefore == 0) {
            qInfo() << "Found" << _iceServerAddresses.count() << "ice-server IP addresses for" << _iceServerAddr;
        }

        if (_iceServerSocket.isNull()) {
            // we don't have a candidate ice-server yet, pick now (without triggering a host lookup since we just did one)
            randomizeICEServerAddress(false);
        }
    }
}

void DomainServer::randomizeICEServerAddress(bool shouldTriggerHostLookup) {
    if (shouldTriggerHostLookup) {
        updateICEServerAddresses();
    }

    // create a list by removing the already failed ice-server addresses
    auto candidateICEAddresses = _iceServerAddresses;

    auto it = candidateICEAddresses.begin();

    while (it != candidateICEAddresses.end()) {
        if (_failedIceServerAddresses.contains(*it)) {
            // we already tried this address and it failed, remove it from list of candidates
            it = candidateICEAddresses.erase(it);
        } else {
            // keep this candidate, it hasn't failed yet
            ++it;
        }
    }

    if (candidateICEAddresses.empty()) {
        // we ended up with an empty list since everything we've tried has failed
        // so clear the set of failed addresses and start going through them again

        qWarning() << "All current ice-server addresses have failed - re-attempting all current addresses for"
                   << _iceServerAddr;

        _failedIceServerAddresses.clear();
        candidateICEAddresses = _iceServerAddresses;
    }

    // of the list of available addresses that we haven't tried, pick a random one
    int maxIndex = candidateICEAddresses.size() - 1;
    int indexToTry = 0;

    if (maxIndex > 0) {
        static std::random_device randomDevice;
        static std::mt19937 generator(randomDevice());
        std::uniform_int_distribution<> distribution(0, maxIndex);

        indexToTry = distribution(generator);
    }

    _iceServerSocket = HifiSockAddr { candidateICEAddresses[indexToTry], ICE_SERVER_DEFAULT_PORT };
    qInfo() << "Set candidate ice-server socket to" << _iceServerSocket;

    // clear our number of hearbeat denials, this should be re-set on ice-server change
    _numHeartbeatDenials = 0;

    // immediately fire an ICE heartbeat once we've picked a candidate ice-server
    sendHeartbeatToIceServer();

    // immediately send an update to the metaverse API when our ice-server changes
    sendICEServerAddressToMetaverseAPI();
}

void DomainServer::setupGroupCacheRefresh() {
    const int REFRESH_GROUPS_INTERVAL_MSECS = 15 * MSECS_PER_SECOND;

    if (!_metaverseGroupCacheTimer) {
        // setup a timer to refresh this server's cached group details
        _metaverseGroupCacheTimer = new QTimer { this };
        connect(_metaverseGroupCacheTimer, &QTimer::timeout, &_gatekeeper, &DomainGatekeeper::refreshGroupsCache);
        _metaverseGroupCacheTimer->start(REFRESH_GROUPS_INTERVAL_MSECS);
    }
}

void DomainServer::maybeHandleReplacementEntityFile() {
    const auto replacementFilePath = getEntitiesReplacementFilePath();
    OctreeUtils::RawEntityData data;
    if (!data.readOctreeDataInfoFromFile(replacementFilePath)) {
        qCWarning(domain_server) << "Replacement file could not be read, it either doesn't exist or is invalid.";
    } else {
        qCDebug(domain_server) << "Replacing existing entity date with replacement file";

        QFile replacementFile(replacementFilePath);
        if (!replacementFile.remove()) {
            // If we can't remove the replacement file, we are at risk of getting into a state where
            // we continually replace the primary entity file with the replacement entity file.
            qCWarning(domain_server) << "Unable to remove replacement file, bailing";
        } else {
            data.resetIdAndVersion();
            auto gzippedData = data.toGzippedByteArray();

            QFile currentFile(getEntitiesFilePath());
            if (!currentFile.open(QIODevice::WriteOnly)) {
                qCWarning(domain_server)
                    << "Failed to update entities data file with replacement file, unable to open entities file for writing";
            } else {
                currentFile.write(gzippedData);
            }
        }
    }
}

void DomainServer::handleOctreeFileReplacement(QByteArray octreeFile) {
    //Assume we have compressed data
    auto compressedOctree = octreeFile;
    QByteArray jsonOctree;

    bool wasCompressed = gunzip(compressedOctree, jsonOctree);
    if (!wasCompressed) {
        // the source was not compressed, assume we were sent regular JSON data
        jsonOctree = compressedOctree;
    }

    OctreeUtils::RawEntityData data;
    if (data.readOctreeDataInfoFromData(jsonOctree)) {
        data.resetIdAndVersion();

        gzip(data.toByteArray(), compressedOctree);

        // write the compressed octree data to a special file
        auto replacementFilePath = getEntitiesReplacementFilePath();
        QFile replacementFile(replacementFilePath);
        if (replacementFile.open(QIODevice::WriteOnly) && replacementFile.write(compressedOctree) != -1) {
            // we've now written our replacement file, time to take the server down so it can
            // process it when it comes back up
            qInfo() << "Wrote octree replacement file to" << replacementFilePath << "- stopping server";

            QMetaObject::invokeMethod(this, "restart", Qt::QueuedConnection);
        } else {
            qWarning() << "Could not write replacement octree data to file - refusing to process";
        }
    } else {
        qDebug() << "Received replacement octree file that is invalid - refusing to process";
    }
}

void DomainServer::handleDomainContentReplacementFromURLRequest(QSharedPointer<ReceivedMessage> message) {
    qInfo() << "Received request to replace content from a url";
    auto node = DependencyManager::get<LimitedNodeList>()->findNodeWithAddr(message->getSenderSockAddr());
    if (node && node->getCanReplaceContent()) {
        // Convert message data into our URL
        QString url(message->getMessage());
        QUrl modelsURL = QUrl(url, QUrl::StrictMode);
        QNetworkAccessManager& networkAccessManager = NetworkAccessManager::getInstance();
        QNetworkRequest request(modelsURL);
        QNetworkReply* reply = networkAccessManager.get(request);

        qDebug() << "Downloading JSON from: " << modelsURL;

        connect(reply, &QNetworkReply::finished, [this, reply, modelsURL]() {
            QNetworkReply::NetworkError networkError = reply->error();
            if (networkError == QNetworkReply::NoError) {
                if (modelsURL.fileName().endsWith(".json.gz")) {
                    handleOctreeFileReplacement(reply->readAll());
                } else if (modelsURL.fileName().endsWith(".zip")) {
                    auto deferred = makePromise("recoverFromUploadedBackup");
                    _contentManager->recoverFromUploadedBackup(deferred, reply->readAll());
                }
            } else {
                qDebug() << "Error downloading JSON from specified file: " << modelsURL;
            }
        });
    }
}

void DomainServer::handleOctreeFileReplacementRequest(QSharedPointer<ReceivedMessage> message) {
    auto node = DependencyManager::get<NodeList>()->nodeWithLocalID(message->getSourceID());
    if (node->getCanReplaceContent()) {
        handleOctreeFileReplacement(message->readAll());
    }
}
