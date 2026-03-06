// Copyright (c) 2011-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/assetfilterproxy.h>
#include <qt/assettablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiconstants.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <assets/assets.h>
#include <assets/assettypes.h>
#include <assets/ans.h>
#include <validation.h>

#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QStatusTipEvent>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <map>

#define DECORATION_SIZE 54
#define NUM_ITEMS 5

Q_DECLARE_METATYPE(interfaces::WalletBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(const PlatformStyle* _platformStyle, QObject* parent = nullptr)
        : QAbstractItemDelegate(parent), platformStyle(_platformStyle)
    {
        connect(this, &TxViewDelegate::width_changed, this, &TxViewDelegate::sizeHintChanged);
    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const override
    {
        painter->save();

        QIcon icon = qvariant_cast<QIcon>(index.data(TransactionTableModel::RawDecorationRole));
        QRect mainRect = option.rect;
        QRect decorationRect(mainRect.topLeft(), QSize(DECORATION_SIZE, DECORATION_SIZE));
        int xspace = DECORATION_SIZE + 8;
        int ypad = 6;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect amountRect(mainRect.left() + xspace, mainRect.top()+ypad, mainRect.width() - xspace, halfheight);
        QRect addressRect(mainRect.left() + xspace, mainRect.top()+ypad+halfheight, mainRect.width() - xspace, halfheight);
        icon = platformStyle->SingleColorIcon(icon);
        icon.paint(painter, decorationRect);

        QDateTime date = index.data(TransactionTableModel::DateRole).toDateTime();
        QString address = index.data(Qt::DisplayRole).toString();
        qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        bool confirmed = index.data(TransactionTableModel::ConfirmedRole).toBool();
        QVariant value = index.data(Qt::ForegroundRole);
        QColor foreground = option.palette.color(QPalette::Text);
        if(value.canConvert<QBrush>())
        {
            QBrush brush = qvariant_cast<QBrush>(value);
            foreground = brush.color();
        }

        painter->setPen(foreground);
        QRect boundingRect;
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &boundingRect);

        if(amount < 0)
        {
            foreground = COLOR_NEGATIVE;
        }
        else if(!confirmed)
        {
            foreground = COLOR_UNCONFIRMED;
        }
        else
        {
            foreground = option.palette.color(QPalette::Text);
        }
        painter->setPen(foreground);
        QString amountText = BitcoinUnits::formatWithUnit(unit, amount, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        if(!confirmed)
        {
            amountText = QString("[") + amountText + QString("]");
        }

        QRect amount_bounding_rect;
        painter->drawText(amountRect, Qt::AlignRight | Qt::AlignVCenter, amountText, &amount_bounding_rect);

        painter->setPen(option.palette.color(QPalette::Text));
        QRect date_bounding_rect;
        painter->drawText(amountRect, Qt::AlignLeft | Qt::AlignVCenter, GUIUtil::dateTimeStr(date), &date_bounding_rect);

        // 0.4*date_bounding_rect.width() is used to visually distinguish a date from an amount.
        const int minimum_width = 1.4 * date_bounding_rect.width() + amount_bounding_rect.width();
        const auto search = m_minimum_width.find(index.row());
        if (search == m_minimum_width.end() || search->second != minimum_width) {
            m_minimum_width[index.row()] = minimum_width;
            Q_EMIT width_changed(index);
        }

        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto search = m_minimum_width.find(index.row());
        const int minimum_text_width = search == m_minimum_width.end() ? 0 : search->second;
        return {DECORATION_SIZE + 8 + minimum_text_width, DECORATION_SIZE};
    }

    BitcoinUnit unit{BitcoinUnit::BTC};

Q_SIGNALS:
    //! An intermediate signal for emitting from the `paint() const` member function.
    void width_changed(const QModelIndex& index) const;

private:
    const PlatformStyle* platformStyle;
    mutable std::map<int, int> m_minimum_width;
};

#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OverviewPage),
    m_platform_style{platformStyle},
    txdelegate(new TxViewDelegate(platformStyle, this))
{
    ui->setupUi(this);

    // use a SingleColorIcon for the "out of sync warning" icon
    QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
    ui->labelTransactionsStatus->setIcon(icon);
    ui->labelWalletStatus->setIcon(icon);

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &TransactionOverviewWidget::clicked, this, &OverviewPage::handleTransactionClicked);

    /** AVN START - asset list setup */
    ui->listAssets->setIconSize(QSize(42, 42));
    ui->listAssets->setMinimumHeight(5 * (42 + 2));
    ui->listAssets->viewport()->setAutoFillBackground(false);

    // Asset search typing delay
    static const int input_filter_delay = 200; // ms
    QTimer* asset_typing_delay = new QTimer(this);
    asset_typing_delay->setSingleShot(true);
    asset_typing_delay->setInterval(input_filter_delay);
    connect(ui->assetSearch, &QLineEdit::textChanged, asset_typing_delay, qOverload<>(&QTimer::start));
    connect(asset_typing_delay, &QTimer::timeout, this, &OverviewPage::assetSearchChanged);

    // Asset context menu
    assetSendAction = new QAction(tr("Send Asset"), this);
    assetIssueSubAction = new QAction(tr("Issue Sub Asset"), this);
    assetIssueUniqueAction = new QAction(tr("Issue Unique Asset"), this);
    assetReissueAction = new QAction(tr("Reissue Asset"), this);
    assetCopyNameAction = new QAction(tr("Copy Name"), this);
    assetCopyAmountAction = new QAction(tr("Copy Amount"), this);
    assetCopyHashAction = new QAction(tr("Copy Hash"), this);
    assetOpenIPFSAction = new QAction(tr("Open IPFS in Browser"), this);
    assetViewANSAction = new QAction(tr("View ANS info"), this);

    assetContextMenu = new QMenu(this);
    assetContextMenu->addAction(assetSendAction);
    assetContextMenu->addAction(assetIssueSubAction);
    assetContextMenu->addAction(assetIssueUniqueAction);
    assetContextMenu->addAction(assetReissueAction);
    assetContextMenu->addSeparator();
    assetContextMenu->addAction(assetOpenIPFSAction);
    assetContextMenu->addAction(assetCopyHashAction);
    assetContextMenu->addSeparator();
    assetContextMenu->addAction(assetViewANSAction);
    assetContextMenu->addSeparator();
    assetContextMenu->addAction(assetCopyNameAction);
    assetContextMenu->addAction(assetCopyAmountAction);

    connect(ui->listAssets, &QListView::customContextMenuRequested, [this](const QPoint& pos) {
        QModelIndex index = ui->listAssets->indexAt(pos);
        if (index.isValid()) {
            handleAssetRightClicked(index);
        }
    });
    ui->listAssets->setContextMenuPolicy(Qt::CustomContextMenu);

    showAssets();
    /** AVN END */

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);
    connect(ui->labelWalletStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
    connect(ui->labelTransactionsStatus, &QPushButton::clicked, this, &OverviewPage::outOfSyncWarningClicked);
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    clientModel->getOptionsModel()->setOption(OptionsModel::OptionID::MaskValues, privacy);
    const auto& balances = walletModel->getCachedBalance();
    if (balances.balance != -1) {
        setBalance(balances);
    }

    ui->listTransactions->setVisible(!m_privacy);

    const QString status_tip = m_privacy ? tr("Privacy mode activated for the Overview tab. To unmask the values, uncheck Settings->Mask values.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    BitcoinUnit unit = walletModel->getOptionsModel()->getDisplayUnit();
    ui->labelBalance->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelUnconfirmed->setText(BitcoinUnits::formatWithPrivacy(unit, balances.unconfirmed_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelImmature->setText(BitcoinUnits::formatWithPrivacy(unit, balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    ui->labelTotal->setText(BitcoinUnits::formatWithPrivacy(unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy));
    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool showImmature = balances.immature_balance != 0;

    ui->labelImmature->setVisible(showImmature);
    ui->labelImmatureText->setVisible(showImmature);
}

void OverviewPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if (model) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());

        connect(model->getOptionsModel(), &OptionsModel::fontForMoneyChanged, this, &OverviewPage::setMonospacedFont);
        setMonospacedFont(clientModel->getOptionsModel()->getFontForMoney());
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if(model && model->getOptionsModel())
    {
        // Set up transaction list
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(model->getTransactionTableModel());
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);

        ui->listTransactions->setModel(filter.get());
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        connect(filter.get(), &TransactionFilterProxy::rowsInserted, this, &OverviewPage::LimitTransactionRows);
        connect(filter.get(), &TransactionFilterProxy::rowsRemoved, this, &OverviewPage::LimitTransactionRows);
        connect(filter.get(), &TransactionFilterProxy::rowsMoved, this, &OverviewPage::LimitTransactionRows);
        LimitTransactionRows();

        // Set up asset list
        assetFilter.reset(new AssetFilterProxy());
        assetFilter->setSourceModel(model->getAssetTableModel());
        assetFilter->sort(AssetTableModel::AssetNameRole, Qt::DescendingOrder);
        ui->listAssets->setModel(assetFilter.get());

        // Keep up to date with wallet
        setBalance(model->getCachedBalance());
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);
    }

    // update the display unit, to not use the default ("AVN")
    updateDisplayUnit();
}

void OverviewPage::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::PaletteChange) {
        QIcon icon = m_platform_style->SingleColorIcon(QStringLiteral(":/icons/warning"));
        ui->labelTransactionsStatus->setIcon(icon);
        ui->labelWalletStatus->setIcon(icon);
    }

    QWidget::changeEvent(e);
}

// Only show most recent NUM_ITEMS rows
void OverviewPage::LimitTransactionRows()
{
    if (filter && ui->listTransactions && ui->listTransactions->model() && filter.get() == ui->listTransactions->model()) {
        for (int i = 0; i < filter->rowCount(); ++i) {
            ui->listTransactions->setRowHidden(i, i >= NUM_ITEMS);
        }
    }
}

void OverviewPage::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        const auto& balances = walletModel->getCachedBalance();
        if (balances.balance != -1) {
            setBalance(balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = walletModel->getOptionsModel()->getDisplayUnit();

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
}

void OverviewPage::setMonospacedFont(const QFont& f)
{
    ui->labelBalance->setFont(f);
    ui->labelUnconfirmed->setFont(f);
    ui->labelImmature->setFont(f);
    ui->labelTotal->setFont(f);
}

/** AVN START */
void OverviewPage::showAssets()
{
    // TODO: Gate on AreAssetsDeployed() once consensus is wired
    bool fShowAssets = true;
    ui->assetFrame->setVisible(fShowAssets);
}

void OverviewPage::assetSearchChanged()
{
    if (assetFilter) {
        assetFilter->setAssetNamePrefix(ui->assetSearch->text());
    }
}

void OverviewPage::handleAssetRightClicked(const QModelIndex& index)
{
    if (!index.isValid() || !assetFilter) return;

    QString assetName = index.data(AssetTableModel::AssetNameRole).toString();
    QString ipfshash = index.data(AssetTableModel::AssetIPFSHashRole).toString();
    QString ansid = index.data(AssetTableModel::AssetANSRole).toString();
    QString ipfsbrowser = "https://cloudflare-ipfs.com/ipfs/%s";

    // Disable send for owner tokens
    if (IsAssetNameAnOwner(assetName.toStdString())) {
        assetName = assetName.left(assetName.size() - 1);
        assetSendAction->setDisabled(true);
    } else {
        assetSendAction->setDisabled(false);
    }

    // Enable/disable IPFS action based on hash availability
    assetOpenIPFSAction->setDisabled(!(ipfshash.size() > 0 && ipfshash.indexOf("Qm") == 0 && ipfsbrowser.indexOf("http") == 0));

    // Enable/disable Copy Hash based on hash availability
    assetCopyHashAction->setDisabled(ipfshash.isEmpty());

    // Enable/disable ANS based on ANS ID availability
    assetViewANSAction->setDisabled(ansid.isEmpty());

    // Enable/disable admin actions based on ownership
    if (!index.data(AssetTableModel::AdministratorRole).toBool()) {
        assetIssueSubAction->setDisabled(true);
        assetIssueUniqueAction->setDisabled(true);
        assetReissueAction->setDisabled(true);
    } else {
        assetIssueSubAction->setDisabled(false);
        assetIssueUniqueAction->setDisabled(false);
        assetReissueAction->setDisabled(true);
        CNewAsset asset;
        auto currentActiveAssetCache = GetCurrentAssetCache();
        if (currentActiveAssetCache && currentActiveAssetCache->GetAssetMetaDataIfExists(assetName.toStdString(), asset)) {
            if (asset.nReissuable)
                assetReissueAction->setDisabled(false);
        }
    }

    // Copy name action
    disconnect(assetCopyNameAction, &QAction::triggered, nullptr, nullptr);
    connect(assetCopyNameAction, &QAction::triggered, [index]() {
        GUIUtil::setClipboard(index.data(AssetTableModel::AssetNameRole).toString());
    });

    // Copy amount action
    disconnect(assetCopyAmountAction, &QAction::triggered, nullptr, nullptr);
    connect(assetCopyAmountAction, &QAction::triggered, [index]() {
        GUIUtil::setClipboard(index.data(AssetTableModel::FormattedAmountRole).toString());
    });

    // Copy hash action
    disconnect(assetCopyHashAction, &QAction::triggered, nullptr, nullptr);
    connect(assetCopyHashAction, &QAction::triggered, [ipfshash]() {
        GUIUtil::setClipboard(ipfshash);
    });

    // Open IPFS action
    disconnect(assetOpenIPFSAction, &QAction::triggered, nullptr, nullptr);
    connect(assetOpenIPFSAction, &QAction::triggered, [ipfshash, ipfsbrowser]() {
        QString url = ipfsbrowser;
        QDesktopServices::openUrl(QUrl::fromUserInput(url.replace("%s", ipfshash)));
    });

    // View ANS action
    disconnect(assetViewANSAction, &QAction::triggered, nullptr, nullptr);
    connect(assetViewANSAction, &QAction::triggered, [this, index, ansid]() {
        if (!ansid.isEmpty()) {
            QString assetname = index.data(AssetTableModel::AssetNameRole).toString();
            CAvianNameSystemID ans(ansid.toStdString());
            QString ansData;
            if (ans.type() == CAvianNameSystemID::ADDR) ansData = "Address: " + QString::fromStdString(ans.addr());
            if (ans.type() == CAvianNameSystemID::IP) ansData = "IPv4: " + QString::fromStdString(ans.ip());
            QMessageBox::information(this, "ANS Info", assetname + " links to:\n" + ansData);
        }
    });

    // Send action
    disconnect(assetSendAction, &QAction::triggered, nullptr, nullptr);
    connect(assetSendAction, &QAction::triggered, [this, index]() {
        Q_EMIT assetSendClicked(assetFilter->mapToSource(index));
    });

    // Issue sub action
    disconnect(assetIssueSubAction, &QAction::triggered, nullptr, nullptr);
    connect(assetIssueSubAction, &QAction::triggered, [this, index]() {
        Q_EMIT assetIssueSubClicked(assetFilter->mapToSource(index));
    });

    // Issue unique action
    disconnect(assetIssueUniqueAction, &QAction::triggered, nullptr, nullptr);
    connect(assetIssueUniqueAction, &QAction::triggered, [this, index]() {
        Q_EMIT assetIssueUniqueClicked(assetFilter->mapToSource(index));
    });

    // Reissue action
    disconnect(assetReissueAction, &QAction::triggered, nullptr, nullptr);
    connect(assetReissueAction, &QAction::triggered, [this, index]() {
        Q_EMIT assetReissueClicked(assetFilter->mapToSource(index));
    });

    assetContextMenu->exec(QCursor::pos());
}
/** AVN END */
