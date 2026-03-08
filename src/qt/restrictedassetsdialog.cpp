// Copyright (c) 2011-2016 The Bitcoin Core developers
// Copyright (c) 2017-2019 The Raven Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/restrictedassetsdialog.h>
#include "ui_restrictedassetsdialog.h"

#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>
#include <qt/assettablemodel.h>
#include <qt/assetfilterproxy.h>

#include <key_io.h>
#include <validation.h>
#include <qt/guiconstants.h>
#include <qt/restrictedassignqualifier.h>
#include "ui_restrictedassignqualifier.h"
#include <qt/restrictedfreezeaddress.h>
#include "ui_restrictedfreezeaddress.h"
#include <qt/sendcoinsdialog.h>
#include <qt/myrestrictedassettablemodel.h>

#include <QGraphicsDropShadowEffect>
#include <QFontMetrics>
#include <QMessageBox>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>
#include <QTimer>
#include <QSortFilterProxyModel>

#include <policy/policy.h>
#include <core_io.h>
#include <wallet/coincontrol.h>

RestrictedAssetsDialog::RestrictedAssetsDialog(const PlatformStyle *_platformStyle, QWidget *parent) :
        QDialog(parent),
        ui(new Ui::RestrictedAssetsDialog),
        clientModel(0),
        model(0),
        platformStyle(_platformStyle)
{
    ui->setupUi(this);
    setWindowTitle("Manage Restricted Assets");
    setupStyling(_platformStyle);
}

void RestrictedAssetsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;
}

void RestrictedAssetsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel()) {
        setBalance(_model->getCachedBalance());
        connect(_model, &WalletModel::balanceChanged, this, &RestrictedAssetsDialog::setBalance);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &RestrictedAssetsDialog::updateDisplayUnit);
        updateDisplayUnit();

        assetFilterProxy = new AssetFilterProxy(this);
        assetFilterProxy->setSourceModel(_model->getAssetTableModel());
        assetFilterProxy->setDynamicSortFilter(true);
        assetFilterProxy->setAssetNamePrefix("$");
        assetFilterProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
        assetFilterProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

        myRestrictedAssetsFilterProxy = new QSortFilterProxyModel(this);
        myRestrictedAssetsFilterProxy->setSourceModel(_model->getMyRestrictedAssetsTableModel());
        myRestrictedAssetsFilterProxy->setDynamicSortFilter(true);
        myRestrictedAssetsFilterProxy->setSortCaseSensitivity(Qt::CaseInsensitive);
        myRestrictedAssetsFilterProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

        myRestrictedAssetsFilterProxy->setSortRole(Qt::EditRole);

        ui->myAddressList->setModel(myRestrictedAssetsFilterProxy);
        ui->myAddressList->horizontalHeader()->setStretchLastSection(true);
        ui->myAddressList->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        ui->myAddressList->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        ui->myAddressList->setAlternatingRowColors(true);
        ui->myAddressList->setSortingEnabled(true);
        ui->myAddressList->verticalHeader()->hide();

        ui->listAssets->setModel(assetFilterProxy);
        ui->listAssets->horizontalHeader()->setStretchLastSection(true);
        ui->listAssets->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        ui->listAssets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        ui->listAssets->setAlternatingRowColors(true);
        ui->listAssets->verticalHeader()->hide();

        AssignQualifier *assignQualifier = new AssignQualifier(platformStyle, this);
        assignQualifier->setWalletModel(_model);
        assignQualifier->setObjectName("tab_assign_qualifier");
        connect(assignQualifier->getUI()->buttonSubmit, SIGNAL(clicked()), this, SLOT(assignQualifierClicked()));
        ui->tabWidget->addTab(assignQualifier, "Assign/Remove Qualifier");

        FreezeAddress *freezeAddress = new FreezeAddress(platformStyle, this);
        freezeAddress->setWalletModel(_model);
        freezeAddress->setObjectName("tab_freeze_address");
        connect(freezeAddress->getUI()->buttonSubmit, SIGNAL(clicked()), this, SLOT(freezeAddressClicked()));
        ui->tabWidget->addTab(freezeAddress, "Restrict Addresses/Global");
    }
}

RestrictedAssetsDialog::~RestrictedAssetsDialog()
{
    delete ui;
}

void RestrictedAssetsDialog::setupStyling(const PlatformStyle *platformStyle)
{
    /** Create the shadow effects on the frames */
    ui->frameAssetBalance->setGraphicsEffect(GUIUtil::getShadowEffect());
    ui->frameAddressList->setGraphicsEffect(GUIUtil::getShadowEffect());
    ui->tabFrame->setGraphicsEffect(GUIUtil::getShadowEffect());
}

QWidget *RestrictedAssetsDialog::setupTabChain(QWidget *prev)
{
    return prev;
}

void RestrictedAssetsDialog::setBalance(const interfaces::WalletBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balances.balance));
    }
}

void RestrictedAssetsDialog::updateDisplayUnit()
{
    setBalance(model->getCachedBalance());
}

void RestrictedAssetsDialog::freezeAddressClicked()
{
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        return;
    }

    // Freeze/unfreeze requires CreateFreezeTransaction / CreateUnfreezeTransaction
    // in wallet/asset_tx.cpp which are not yet ported
    QMessageBox msgBox;
    msgBox.setText(tr("Freeze/unfreeze functionality requires restricted asset transaction support which is not yet ported."));
    msgBox.exec();
}

void RestrictedAssetsDialog::assignQualifierClicked()
{
    WalletModel::UnlockContext ctx(model->requestUnlock());
    if(!ctx.isValid())
    {
        return;
    }

    // Assign/remove qualifier requires CreateAssignQualifierTransaction
    // in wallet/asset_tx.cpp which is not yet ported
    QMessageBox msgBox;
    msgBox.setText(tr("Assign/remove qualifier functionality requires restricted asset transaction support which is not yet ported."));
    msgBox.exec();
}
