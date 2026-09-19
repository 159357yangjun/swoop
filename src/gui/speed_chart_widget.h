#ifndef SPEED_CHART_WIDGET_H
#define SPEED_CHART_WIDGET_H

#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QWidget>
#include <QVector>

// 速度曲线图组件：用 Qt Charts 绘制实时下载速度曲线
// 用法：定时调用 addDataPoint(speedBps) 添加数据点，图表自动滚动更新
// 可嵌入任意 QWidget（如 TaskDetailDialog 的标签页、MainWindow 的停靠面板）
class SpeedChartWidget : public QWidget {
    Q_OBJECT
public:
    explicit SpeedChartWidget(QWidget* parent = nullptr);
    ~SpeedChartWidget();

    // 添加一个速度数据点（单位 B/s），图表自动保持最近 maxPoints 个点
    void addDataPoint(qreal speedBps);

    // 清空历史数据
    void clear();

    // 设置最大数据点数（默认 120 = 60秒 × 500ms 采样）
    void setMaxPoints(int n);

private:
    void setupChart();

    QChart*      m_chart;
    QLineSeries* m_series;
    QChartView*  m_chartView;

    QVector<qreal> m_data;   // 速度数据缓冲（B/s）
    int            m_maxPoints = 120;
    int            m_xIndex    = 0;   // X 轴计数器
    qreal          m_maxSpeed  = 0;   // Y 轴自适应最大值
};

#endif // SPEED_CHART_WIDGET_H
