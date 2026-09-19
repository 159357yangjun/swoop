#include "speed_chart_widget.h"
#include "logger.h"

#include <QVBoxLayout>
#include <QValueAxis>

SpeedChartWidget::SpeedChartWidget(QWidget* parent)
    : QWidget(parent)
{
    setupChart();
}

SpeedChartWidget::~SpeedChartWidget() = default;

void SpeedChartWidget::setupChart()
{
    m_chart = new QChart();
    m_series = new QLineSeries();

    // 线条样式
    QPen pen(QColor("#2196F3"));
    pen.setWidth(2);
    m_series->setPen(pen);

    m_chart->addSeries(m_series);
    m_chart->setTitle(QStringLiteral("下载速度 (B/s)"));
    m_chart->legend()->hide();

    // X 轴：时间（秒），显示最近 60 秒
    auto* axisX = new QValueAxis;
    axisX->setRange(0, m_maxPoints / 2);  // 500ms 采样 → 120 点 = 60 秒
    axisX->setLabelFormat(QStringLiteral("%d"));
    axisX->setTitleText(QStringLiteral("时间 (秒)"));
    m_chart->addAxis(axisX, Qt::AlignBottom);
    m_series->attachAxis(axisX);

    // Y 轴：速度 (B/s)，自适应
    auto* axisY = new QValueAxis;
    axisY->setRange(0, 1024);  // 初始 1KB/s，随数据自动扩展
    axisY->setLabelFormat(QStringLiteral("%d"));
    axisY->setTitleText(QStringLiteral("速度 (B/s)"));
    m_chart->addAxis(axisY, Qt::AlignLeft);
    m_series->attachAxis(axisY);

    // 图表视图
    m_chartView = new QChartView(m_chart, this);
    m_chartView->setRenderHint(QPainter::Antialiasing);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_chartView);
}

void SpeedChartWidget::addDataPoint(qreal speedBps)
{
    // 添加新数据点
    m_series->append(m_xIndex / 2.0, speedBps);  // X = 秒数（每 0.5s 一个点）
    m_data.append(speedBps);
    ++m_xIndex;

    // 超过最大点数时，移除最旧的点
    if (m_data.size() > m_maxPoints) {
        m_data.removeFirst();
        // 重建 series（QLineSeries 没有高效移除头部点的 API）
        m_series->clear();
        int startX = m_xIndex - m_data.size();
        for (int i = 0; i < m_data.size(); ++i)
            m_series->append((startX + i) / 2.0, m_data[i]);
    }

    // Y 轴自适应：取当前数据最大值向上取整到 KB
    qreal currentMax = 0;
    for (qreal v : m_data)
        if (v > currentMax) currentMax = v;

    if (currentMax > m_maxSpeed)
        m_maxSpeed = currentMax;
    else if (currentMax < m_maxSpeed * 0.7)
        m_maxSpeed = currentMax;  // 速度下降时跟随

    // Y 轴范围向上留 20% 余量
    qreal yMax = m_maxSpeed * 1.2;
    if (yMax < 1024) yMax = 1024;  // 最小 1KB

    auto* axisY = qobject_cast<QValueAxis*>(m_chart->axes(Qt::Vertical).value(0));
    if (axisY)
        axisY->setRange(0, yMax);

    // X 轴滚动
    auto* axisX = qobject_cast<QValueAxis*>(m_chart->axes(Qt::Horizontal).value(0));
    if (axisX) {
        qreal xStart = (m_xIndex > m_maxPoints) ? (m_xIndex - m_maxPoints) / 2.0 : 0;
        axisX->setRange(xStart, xStart + m_maxPoints / 2.0);
    }
}

void SpeedChartWidget::clear()
{
    m_series->clear();
    m_data.clear();
    m_xIndex = 0;
    m_maxSpeed = 0;

    auto* axisY = qobject_cast<QValueAxis*>(m_chart->axes(Qt::Vertical).value(0));
    if (axisY)
        axisY->setRange(0, 1024);

    auto* axisX = qobject_cast<QValueAxis*>(m_chart->axes(Qt::Horizontal).value(0));
    if (axisX)
        axisX->setRange(0, m_maxPoints / 2);
}

void SpeedChartWidget::setMaxPoints(int n)
{
    m_maxPoints = n;
    clear();
}
