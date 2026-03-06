// Copyright (c) 2022 The Avian Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/duster.h>
#include <qt/forms/ui_duster.h>

#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <interfaces/wallet.h>

#include <QApplication>
#include <QDateTime>
#include <QHeaderView>
#include <QMessageBox>
#include <QProgressDialog>
#include <QThread>

DusterDialog::DusterDialog(const PlatformStyle* _platformStyle, QWidget* parent) : QDialog(parent),
                                                                                   ui(new Ui::DusterDialog),
                                                                                   platformStyle(_platformStyle),
                                                                                   model(nullptr)
{
    // Setup the UI
    ui->setupUi(this);
    ui->dustAddress->setReadOnly(true);

    // Set default values for amount fields
    ui->minInputAmount->setValue(1000000);       // 0.01 AVN in satoshis
    ui->maxInputAmount->setValue(2500000000);    // 25 AVN in satoshis
    ui->maxBatchAmount->setValue(1000000000000); // 10,000 AVN in satoshis

    // Use the table and info label from the UI file
    blocksTable = ui->blocksTable;
    infoLabel = ui->infoLabel;

    // Configure the table properties
    createBlockList();

    // Connect UI elements
    connect(ui->refreshButton, &QPushButton::clicked, this, &DusterDialog::updateBlockList);
    connect(ui->consolidateButton, &QPushButton::clicked, this, &DusterDialog::compactBlocks);

    ui->addressBookButton->setIcon(platformStyle->SingleColorIcon(":/icons/address-book"));

    // Load settings - mimicking Python script limits
    minimumBlockAmount = 3;
    blockDivisor = 500;
}

DusterDialog::~DusterDialog()
{
    delete ui;
}

void DusterDialog::setModel(WalletModel* _model)
{
    this->model = _model;
}

void DusterDialog::createBlockList()
{
    blocksTable->setColumnCount(9);

    QStringList headers;
    headers << tr("Address") << tr("Amount") << tr("Confirmations") << tr("Date") << tr("Details")
            << tr("Label") << tr("Amount64") << tr("Vout") << tr("Size");
    blocksTable->setHorizontalHeaderLabels(headers);

    blocksTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    blocksTable->setSelectionMode(QAbstractItemView::NoSelection);
    blocksTable->setShowGrid(false);
    blocksTable->setAlternatingRowColors(true);

    // Hide internal columns
    blocksTable->hideColumn(COLUMN_LABEL);
    blocksTable->hideColumn(COLUMN_AMOUNT_INT64);
    blocksTable->hideColumn(COLUMN_VOUT_INDEX);
    blocksTable->hideColumn(COLUMN_INPUT_SIZE);

    // Set column widths for visible columns
    blocksTable->horizontalHeader()->setStretchLastSection(true);
    blocksTable->horizontalHeader()->resizeSection(COLUMN_ADDRESS, 240);
    blocksTable->horizontalHeader()->resizeSection(COLUMN_AMOUNT, 120);
    blocksTable->horizontalHeader()->resizeSection(COLUMN_CONFIRMATIONS, 100);
    blocksTable->horizontalHeader()->resizeSection(COLUMN_DATE, 150);
}

void DusterDialog::updateBlockList()
{
    // Prepare to refresh
    blocksTable->setRowCount(0);
    blocksTable->setEnabled(false);
    blocksTable->setAlternatingRowColors(true);

    if (!model) {
        infoLabel->setText(tr("No wallet model available."));
        blocksTable->setEnabled(true);
        return;
    }

    BitcoinUnit nDisplayUnit = BitcoinUnit::BTC;
    if (model->getOptionsModel())
        nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    // TODO: Port UTXO listing from wallet interface
    // BTC 30.2 uses model->wallet().listCoins() which returns different types
    // than old Avian's model->listCoins(mapCoins)
    //
    // For now, show a placeholder message
    infoLabel->setText(tr("UTXO listing not yet implemented for BTC 30.2 wallet interface. Coming soon."));
    blocksTable->setEnabled(true);
}

void DusterDialog::on_addressBookButton_clicked()
{
    if (!model)
        return;

    AddressBookPage dlg(platformStyle, AddressBookPage::ForSelection, AddressBookPage::ReceivingTab, this);
    dlg.setModel(model->getAddressTableModel());

    if (dlg.exec()) {
        ui->dustAddress->setText(dlg.getReturnValue());
    }
}

void DusterDialog::compactBlocks()
{
    // Safety check: ensure we have a model
    if (!model) {
        QMessageBox::warning(this, tr("UTXO Consolidation"), tr("No wallet model available."), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Safety check: ensure we have a destination address
    if (ui->dustAddress->text().isEmpty()) {
        QMessageBox::warning(this, tr("UTXO Consolidation"), tr("Please select a destination address first."), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // Check number of blocks
    if (blocksTable->rowCount() <= minimumBlockAmount) {
        QMessageBox::information(this, tr("UTXO Consolidation"), tr("The wallet is already optimized."), QMessageBox::Ok, QMessageBox::Ok);
        return;
    }

    // TODO: Port consolidation transaction creation to BTC 30.2 wallet interface
    // The old code used:
    //   - model->listCoins(mapCoins) to get UTXOs
    //   - CCoinControl to select specific inputs
    //   - WalletModelTransaction for building the transaction
    //   - model->prepareTransaction() and model->sendCoins() for signing and broadcasting
    //
    // BTC 30.2 wallet interface is significantly different and needs adaptation

    QMessageBox::information(this, tr("UTXO Consolidation"),
        tr("UTXO consolidation transaction creation is not yet implemented for BTC 30.2 wallet interface. Coming soon."),
        QMessageBox::Ok, QMessageBox::Ok);
}

QString DusterDialog::strPad(QString s, int nPadLength, QString sPadding)
{
    while (s.length() < nPadLength) {
        s = sPadding + s;
    }
    return s;
}

void DusterDialog::sortView(int column, Qt::SortOrder order)
{
    sortColumn = column;
    sortOrder = order;
    blocksTable->sortByColumn(column, order);
    blocksTable->horizontalHeader()->setSortIndicator((sortColumn == COLUMN_AMOUNT_INT64 ? 0 : sortColumn), sortOrder);
}

void DusterDialog::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
}

void DusterDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    updateBlockList();
}
