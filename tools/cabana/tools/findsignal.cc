#include "tools/cabana/tools/findsignal.h"

#include <QDoubleValidator>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QtConcurrent>
#include <QTimer>
#include <QVBoxLayout>

// FindSignalModel

QVariant FindSignalModel::headerData(int section, Qt::Orientation orientation, int role) const {
  static QString titles[] = {"Id", "Start Bit, size", "(time, value)"};
  if (role != Qt::DisplayRole) return {};
  return orientation == Qt::Horizontal ? titles[section] : QString::number(section + 1);
}

QVariant FindSignalModel::data(const QModelIndex &index, int role) const {
  if (role == Qt::DisplayRole) {
    const auto &s = filtered_signals[index.row()];
    switch (index.column()) {
      case 0: return s.id.toString();
      case 1: return QString("%1, %2").arg(s.sig.start_bit).arg(s.sig.size);
      case 2: return s.values.join(" ");
    }
  }
  return {};
}

void FindSignalModel::search(std::function<bool(double)> cmp) {
  beginResetModel();

  std::mutex lock;
  const auto prev_sigs = !histories.isEmpty() ? histories.back() : initial_signals;
  filtered_signals.clear();
  filtered_signals.reserve(prev_sigs.size());
  QtConcurrent::blockingMap(prev_sigs, [&](auto &s) {
    const auto &events = can->events(s.id);
    auto first = std::upper_bound(events.cbegin(), events.cend(), s.mono_time, [](uint64_t ts, auto &e) { return ts < e->mono_time; });
    auto it = std::find_if(first, events.cend(), [&](auto e) { return cmp(get_raw_value(e->dat, e->size, s.sig)); });
    if (it != events.cend()) {
      auto values = s.values;
      values += QString("(%1, %2)").arg((*it)->mono_time / 1e9 - can->routeStartTime(), 0, 'f', 2).arg(get_raw_value((*it)->dat, (*it)->size, s.sig));
      std::lock_guard lk(lock);
      filtered_signals.push_back({.id = s.id, .mono_time = (*it)->mono_time, .sig = s.sig, .values = values});
    }
  });
  histories.push_back(filtered_signals);

  endResetModel();
}

void FindSignalModel::undo() {
  if (!histories.isEmpty()) {
    beginResetModel();
    histories.pop_back();
    filtered_signals.clear();
    if (!histories.isEmpty()) filtered_signals = histories.back();
    endResetModel();
  }
}

void FindSignalModel::reset() {
  beginResetModel();
  histories.clear();
  filtered_signals.clear();
  initial_signals.clear();
  endResetModel();
}

// FindSignalDlg
FindSignalDlg::FindSignalDlg(QWidget *parent) : QDialog(parent, Qt::WindowFlags() | Qt::Window) {
  setWindowTitle(tr("Find Signal"));
  setAttribute(Qt::WA_DeleteOnClose);
  QVBoxLayout *main_layout = new QVBoxLayout(this);

  properties_group = new QGroupBox(tr("Signal Properties"));
  QFormLayout *property_layout = new QFormLayout(properties_group);
  property_layout->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);

  QHBoxLayout *hlayout = new QHBoxLayout();
  hlayout->addWidget(min_size = new QSpinBox);
  hlayout->addWidget(new QLabel("-"));
  hlayout->addWidget(max_size = new QSpinBox);
  hlayout->addWidget(litter_endian = new QCheckBox(tr("Little endian")));
  hlayout->addWidget(is_signed = new QCheckBox(tr("Signed")));
  hlayout->addStretch(0);
  min_size->setRange(1, 64);
  max_size->setRange(1, 64);
  litter_endian->setChecked(true);
  property_layout->addRow(tr("Size"), hlayout);
  property_layout->addRow(tr("Factor"), factor_edit = new QLineEdit("1.0"));
  property_layout->addRow(tr("Offset"), offset_edit = new QLineEdit("0.0"));

  QGroupBox *find_group = new QGroupBox(tr("Find signal"), this);
  QVBoxLayout *vlayout = new QVBoxLayout(find_group);
  hlayout = new QHBoxLayout();
  hlayout->addWidget(new QLabel(tr("Value")));
  hlayout->addWidget(compare_cb = new QComboBox(this));
  hlayout->addWidget(value1 = new QLineEdit);
  hlayout->addWidget(to_label = new QLabel("-"));
  hlayout->addWidget(value2 = new QLineEdit);
  hlayout->addWidget(undo_btn = new QPushButton(tr("Undo prev find"), this));
  hlayout->addWidget(search_btn = new QPushButton(tr("Find")));
  hlayout->addWidget(reset_btn = new QPushButton(tr("Reset"), this));
  vlayout->addLayout(hlayout);

  compare_cb->addItems({"=", ">", ">=", "!=", "<", "<=", "between"});
  value2->setVisible(false);
  to_label->setVisible(false);
  undo_btn->setEnabled(false);
  reset_btn->setEnabled(false);

  auto double_validator = new QDoubleValidator(this);
  double_validator->setLocale(QLocale::C);
  for (auto edit : {value1, value2, factor_edit, offset_edit}) {
    edit->setValidator(double_validator);
  }

  vlayout->addWidget(view = new QTableView(this));
  vlayout->addWidget(stats_label = new QLabel());
  view->setContextMenuPolicy(Qt::CustomContextMenu);
  view->horizontalHeader()->setStretchLastSection(true);
  view->horizontalHeader()->setSelectionMode(QAbstractItemView::NoSelection);
  view->setSelectionBehavior(QAbstractItemView::SelectRows);
  view->setModel(model = new FindSignalModel(this));

  main_layout->addWidget(properties_group);
  main_layout->addWidget(find_group);

  setMinimumSize({700, 550});
  QObject::connect(search_btn, &QPushButton::clicked, this, &FindSignalDlg::search);
  QObject::connect(undo_btn, &QPushButton::clicked, model, &FindSignalModel::undo);
  QObject::connect(model, &QAbstractItemModel::modelReset, this, &FindSignalDlg::modelReset);
  QObject::connect(reset_btn, &QPushButton::clicked, model, &FindSignalModel::reset);
  QObject::connect(view, &QTableView::customContextMenuRequested, this, &FindSignalDlg::customMenuRequested);
  QObject::connect(view, &QTableView::doubleClicked, [this](const QModelIndex &index) {
    if (index.isValid()) emit openMessage(model->filtered_signals[index.row()].id);
  });
  QObject::connect(compare_cb, qOverload<int>(&QComboBox::currentIndexChanged), [=](int index) {
    to_label->setVisible(index == compare_cb->count() - 1);
    value2->setVisible(index == compare_cb->count() - 1);
  });
}

void FindSignalDlg::search() {
  if (model->histories.isEmpty()) {
    setInitialSignals();
  }
  auto v1 = value1->text().toDouble();
  auto v2 = value2->text().toDouble();
  std::function<bool(double)> cmp = nullptr;
  switch (compare_cb->currentIndex()) {
    case 0: cmp = [v1](double v) { return v == v1;}; break;
    case 1: cmp = [v1](double v) { return v > v1;};break;
    case 2: cmp = [v1](double v) { return v >= v1;};break;
    case 3: cmp = [v1](double v) { return v != v1;}; break;
    case 4: cmp = [v1](double v) { return v < v1;}; break;
    case 5: cmp = [v1](double v) { return v <= v1;}; break;
    case 6: cmp = [v1, v2](double v) { return v >= v1 && v <= v2;}; break;
  }
  properties_group->setEnabled(false);
  search_btn->setEnabled(false);
  stats_label->setVisible(false);
  search_btn->setText("Finding ....");
  QTimer::singleShot(0, [=]() { model->search(cmp); });
}

void FindSignalDlg::setInitialSignals() {
  cabana::Signal sig{};
  sig.is_little_endian = litter_endian->isChecked();
  sig.is_signed = is_signed->isChecked();
  sig.factor = factor_edit->text().toDouble();
  sig.offset = offset_edit->text().toDouble();

  model->initial_signals.clear();
  for (auto it = can->last_msgs.cbegin(); it != can->last_msgs.cend(); ++it) {
    const int total_size = it.value().dat.size() * 8;
    for (int size = min_size->value(); size <= max_size->value(); ++size) {
      for (int start = 0; start <= total_size - size; ++start) {
        FindSignalModel::SearchSignal s{.id = it.key(), .mono_time = 0, .sig = sig};
        updateSigSizeParamsFromRange(s.sig, start, size);
        model->initial_signals.push_back(s);
      }
    }
  }
}

void FindSignalDlg::modelReset() {
  properties_group->setEnabled(model->histories.isEmpty());
  search_btn->setText(model->histories.isEmpty() ? tr("Find") : tr("Find Next"));
  reset_btn->setEnabled(!model->histories.isEmpty());
  undo_btn->setEnabled(model->histories.size() > 1);
  search_btn->setEnabled(model->rowCount() > 0 || model->histories.isEmpty());
  stats_label->setVisible(true);
  stats_label->setText(tr("%1 matches. right click on an item to create signal. double click to open message").arg(model->filtered_signals.size()));
}

void FindSignalDlg::customMenuRequested(const QPoint &pos) {
  if (auto index = view->indexAt(pos); index.isValid()) {
    QMenu menu(this);
    menu.addAction(tr("Create Signal"));
    if (menu.exec(view->mapToGlobal(pos))) {
      auto &s = model->filtered_signals[index.row()];
      auto msg = dbc()->msg(s.id);
      if (!msg) {
        UndoStack::push(new EditMsgCommand(s.id, dbc()->newMsgName(s.id), can->lastMessage(s.id).dat.size()));
        msg = dbc()->msg(s.id);
      }
      s.sig.name = dbc()->newSignalName(s.id);
      UndoStack::push(new AddSigCommand(s.id, s.sig));
      emit openMessage(s.id);
    }
  }
}
