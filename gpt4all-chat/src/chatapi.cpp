#include "chatapi.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>
#include <QUtf8StringView>
#include <QVariant>
#include <Qt>
#include <QtGlobal>
#include <QtLogging>

#include <functional>
#include <iostream>
#include <utility>

using namespace Qt::Literals::StringLiterals;

//#define DEBUG

ChatAPI::ChatAPI()
    : QObject(nullptr)
    , m_modelName("gpt-3.5-turbo")
    , m_requestURL("")
    , m_responseCallback(nullptr)
{
}

size_t ChatAPI::requiredMem(const std::string &modelPath, int n_ctx, int ngl)
{
    Q_UNUSED(modelPath);
    Q_UNUSED(n_ctx);
    Q_UNUSED(ngl);
    return 0;
}

bool ChatAPI::loadModel(const std::string &modelPath, int n_ctx, int ngl)
{
    Q_UNUSED(modelPath);
    Q_UNUSED(n_ctx);
    Q_UNUSED(ngl);
    return true;
}

void ChatAPI::setThreadCount(int32_t n_threads)
{
    Q_UNUSED(n_threads);
}

int32_t ChatAPI::threadCount() const
{
    return 1;
}

ChatAPI::~ChatAPI()
{
}

bool ChatAPI::isModelLoaded() const
{
    return true;
}

void ChatAPI::prompt(
    std::string_view        prompt,
    const PromptCallback   &promptCallback,
    const ResponseCallback &responseCallback,
    const PromptContext    &promptCtx,
    bool                    allowContextShift
) {
    Q_UNUSED(promptCallback);
    Q_UNUSED(allowContextShift);

    if (!isModelLoaded())
        throw std::invalid_argument("Attempted to prompt an unloaded model.");
    if (!promptCtx.n_predict)
        return; // nothing requested

    QString formattedPrompt = QUtf8StringView(prompt).toString();

    // FIXME: We don't set the max_tokens on purpose because in order to do so safely without encountering
    // an error we need to be able to count the tokens in our prompt. The only way to do this is to use
    // the OpenAI tiktoken library or to implement our own tokenization function that matches precisely
    // the tokenization used by the OpenAI model we're calling. OpenAI has not introduced any means of
    // using the REST API to count tokens in a prompt.
    QJsonObject root {
        { "model",       m_modelName     },
        { "stream",      true            },
        { "temperature", promptCtx.temp  },
        { "top_p",       promptCtx.top_p },
    };

    // conversation history
    // TODO(jared): Use QXmlStreamReader to break XML-encoded message pairs into role/content pairs
    QJsonArray messages;
    for (int i = 0; i < 1; ++i) {
        messages.append(QJsonObject {
            { "role",    i % 2 == 0 ? "user" : "assistant" },
            { "content", "TODO"                            },
        });
    }
    root.insert("messages", messages);

    QJsonDocument doc(root);

#if defined(DEBUG)
    qDebug().noquote() << "ChatAPI::prompt begin network request" << doc.toJson();
#endif

    m_responseCallback = responseCallback;

    // The following code sets up a worker thread and object to perform the actual api request to
    // chatgpt and then blocks until it is finished
    QThread workerThread;
    ChatAPIWorker worker(this);
    worker.moveToThread(&workerThread);
    connect(&worker, &ChatAPIWorker::finished, &workerThread, &QThread::quit, Qt::DirectConnection);
    connect(this, &ChatAPI::request, &worker, &ChatAPIWorker::request, Qt::QueuedConnection);
    workerThread.start();
    emit request(m_apiKey, doc.toJson(QJsonDocument::Compact));
    workerThread.wait();

    m_responseCallback = nullptr;

#if defined(DEBUG)
    qDebug() << "ChatAPI::prompt end network request";
#endif
}

bool ChatAPI::callResponse(int32_t token, const std::string& string)
{
    Q_ASSERT(m_responseCallback);
    if (!m_responseCallback) {
        std::cerr << "ChatAPI ERROR: no response callback!\n";
        return false;
    }
    return m_responseCallback(token, string);
}

void ChatAPIWorker::request(const QString &apiKey, const QByteArray &array)
{
    QUrl apiUrl(m_chat->url());
    const QString authorization = u"Bearer %1"_s.arg(apiKey).trimmed();
    QNetworkRequest request(apiUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", authorization.toUtf8());
#if defined(DEBUG)
    qDebug() << "ChatAPI::request"
             << "API URL: " << apiUrl.toString()
             << "Authorization: " << authorization.toUtf8();
#endif
    m_networkManager = new QNetworkAccessManager(this);
    QNetworkReply *reply = m_networkManager->post(request, array);
    connect(qGuiApp, &QCoreApplication::aboutToQuit, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this, &ChatAPIWorker::handleFinished);
    connect(reply, &QNetworkReply::readyRead, this, &ChatAPIWorker::handleReadyRead);
    connect(reply, &QNetworkReply::errorOccurred, this, &ChatAPIWorker::handleErrorOccurred);
}

void ChatAPIWorker::handleFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        emit finished();
        return;
    }

    QVariant response = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);

    if (!response.isValid()) {
        m_chat->callResponse(
            -1,
            tr("ERROR: Network error occurred while connecting to the API server")
                .toStdString()
        );
        return;
    }

    bool ok;
    int code = response.toInt(&ok);
    if (!ok || code != 200) {
        bool isReplyEmpty(reply->readAll().isEmpty());
        if (isReplyEmpty)
            m_chat->callResponse(
                -1,
                tr("ChatAPIWorker::handleFinished got HTTP Error %1 %2")
                    .arg(code)
                    .arg(reply->errorString())
                    .toStdString()
            );
        qWarning().noquote() << "ERROR: ChatAPIWorker::handleFinished got HTTP Error" << code << "response:"
                             << reply->errorString();
    }
    reply->deleteLater();
    emit finished();
}

void ChatAPIWorker::handleReadyRead()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        emit finished();
        return;
    }

    QVariant response = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);

    if (!response.isValid())
        return;

    bool ok;
    int code = response.toInt(&ok);
    if (!ok || code != 200) {
        m_chat->callResponse(
            -1,
            u"ERROR: ChatAPIWorker::handleReadyRead got HTTP Error %1 %2: %3"_s
                .arg(code).arg(reply->errorString(), reply->readAll()).toStdString()
        );
        emit finished();
        return;
    }

    while (reply->canReadLine()) {
        QString jsonData = reply->readLine().trimmed();
        if (jsonData.startsWith("data:"))
            jsonData.remove(0, 5);
        jsonData = jsonData.trimmed();
        if (jsonData.isEmpty())
            continue;
        if (jsonData == "[DONE]")
            continue;
#if defined(DEBUG)
        qDebug().noquote() << "line" << jsonData;
#endif
        QJsonParseError err;
        const QJsonDocument document = QJsonDocument::fromJson(jsonData.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            m_chat->callResponse(-1, u"ERROR: ChatAPI responded with invalid json \"%1\""_s
                                         .arg(err.errorString()).toStdString());
            continue;
        }

        const QJsonObject root = document.object();
        const QJsonArray choices = root.value("choices").toArray();
        const QJsonObject choice = choices.first().toObject();
        const QJsonObject delta = choice.value("delta").toObject();
        const QString content = delta.value("content").toString();
        m_currentResponse += content;
        if (!m_chat->callResponse(0, content.toStdString())) {
            reply->abort();
            emit finished();
            return;
        }
    }
}

void ChatAPIWorker::handleErrorOccurred(QNetworkReply::NetworkError code)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply->error() == QNetworkReply::OperationCanceledError /*when we call abort on purpose*/) {
        emit finished();
        return;
    }

    qWarning().noquote() << "ERROR: ChatAPIWorker::handleErrorOccurred got HTTP Error" << code << "response:"
                         << reply->errorString();
    emit finished();
}
